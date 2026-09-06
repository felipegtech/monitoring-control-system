# Service Specification

Companion to `protocol.md`. That document defines *what bytes* each message
carries; this one defines *when* they're sent, *what the server does* with
them, and *how concurrent access to shared server state is kept safe*.

## Service primitives

Before the byte-level format, it's worth stating the service the protocol
offers independently of its wire encoding — the operations a *user* of the
protocol (a node, or an admin client) can invoke, the way an RFC describes
primitives like `SEND`/`RECEIVE` before getting into packet formats.

| Primitive | Invoked by | Effect | Reply |
|---|---|---|---|
| `REGISTER(node_name, node_type, interval_hint)` | Node | Registers the node in the server's registry, obtaining a numeric identity | `RESP(assigned_node_id)` or `ERROR` |
| `EVENT(event_code, severity, message)` | Node | Reports a critical, one-off occurrence tied to an already-registered node | `RESP` (acknowledgment) or `ERROR` |
| `TELEMETRY(metric_type, value)` | Node | Reports one periodic reading | none — deliberately fire-and-forget (see `transport-analysis.md`) |
| `AUTH(username, password)` | Admin client | Authenticates the client against real OS user accounts via PAM | `RESP(session_id)` or `ERROR(AUTH_FAILED)` |
| `QUERY(query_type, target_node_id, count)` | Admin client | Requests one of STATUS / HISTORY / LIST_NODES | `RESP(...)` or `ERROR` |

Every primitive except `TELEMETRY` is **confirmed**: the caller always gets
back exactly one `RESP` or `ERROR`, never silence. That's a property of the
*service* itself, not an accident of implementation — it's the reason
REGISTER/EVENT/AUTH/QUERY all sit on TCP: a primitive that promises a reply
needs a transport with a natural place to wait for one. `TELEMETRY` is the
only primitive that intentionally makes no such promise, which is exactly
why it's the only one on UDP.

## Node lifecycle

Two different state machines are in play here, from two different vantage
points — worth keeping straight since they use overlapping words for
different things:

**The node's own view of itself** (internal to the node's implementation,
never transmitted as a field):

```
DISCONNECTED --sends REGISTER--> REGISTERING --receives RESP--> ACTIVE
ACTIVE --TCP connection drops--> DISCONNECTED
```

**The server's tracked view of that node** (this is what's actually stored
in the registry and reported back by QUERY/STATUS — matches the state enum
in `protocol.md`):

```
(no row exists — reported as ERROR UNKNOWN_NODE, not a state) --REGISTER succeeds--> REGISTERED
REGISTERED --first TELEMETRY arrives--> ACTIVE
ACTIVE --no TELEMETRY within timeout--> STALE
STALE --TELEMETRY resumes--> ACTIVE
REGISTERED, ACTIVE, or STALE --TCP connection drops--> DISCONNECTED
```

**Note on `DISCONNECTED`:** the first sketch of this design described the
server's tracked chain as `UNKNOWN → REGISTERED → ACTIVE → STALE`, with no
explicit `DISCONNECTED`. Adding it is a deliberate, called-out extension:
without it, a node whose TCP connection drops would stay reported as
`REGISTERED`/`ACTIVE`/`STALE` forever. `UNKNOWN` itself needs no state code
at all — a node id the server has never seen simply has no registry row,
which is exactly what the `UNKNOWN_NODE` error code already covers.

The normative version of both machines, with an explicit event/condition/
action table per transition, is `maquinas-de-estado.md` § 1 and § 2. The
sketches above are the summary; that document is what an implementer
follows.

Sequence of events:

1. Node opens a TCP connection to the server and sends REGISTER
   (`entity_id=0`).
2. Server checks the requested `node_name` isn't already in use by another
   *currently connected* node, assigns a new numeric `node_id`, stores the
   node in its registry as `REGISTERED`, and replies RESP with the assigned
   id.
3. The node keeps that TCP connection open for the rest of its life — it's
   both the channel for future EVENT messages and the thing whose closing
   is how the server ever finds out the node is gone. (This is *why*
   REGISTER/EVENT are TCP at all: TCP's connection state is what makes
   disconnect-detection possible. UDP has no concept of "the client hung
   up.")
4. The node starts sending TELEMETRY over UDP, independently of the TCP
   connection, using its assigned `node_id` as `entity_id`. The moment the
   *first* TELEMETRY datagram arrives, the server promotes that node from
   `REGISTERED` to `ACTIVE` — `REGISTERED` is specifically the (usually
   brief) window where a node has completed the handshake but hasn't
   proven yet that its telemetry channel actually works.
5. If the TCP connection closes (node crashes, network drops, clean
   shutdown) — the server's `recv()` on that connection returns 0 or an
   error — the node transitions to `DISCONNECTED` regardless of which of
   `REGISTERED`/`ACTIVE`/`STALE` it was in. The server must then also stop
   trusting any further UDP TELEMETRY carrying that `node_id` until the
   node re-REGISTERs (a stale node_id could otherwise let a *different*
   process spoof telemetry for a node that's no longer really there).

## STALE detection

A node can have its TCP connection perfectly alive (so it's not
DISCONNECTED) while its telemetry silently stops arriving. STALE exists
because TELEMETRY is UDP and inherently can't tell you when it stops — no
FIN, no RST, nothing. The server has to notice the *absence* of packets
itself.

Mechanism: each node record stores `last_telemetry_at` (server's own receive
timestamp, not the node's) and `telemetry_interval_hint` (from REGISTER).
A node is considered STALE once `now - last_telemetry_at > 3 *
telemetry_interval_hint`. Why the multiplier is 3 and not 2 or 5, and what
the server substitutes when `interval_hint` arrives as `0`, are specified
in `maquinas-de-estado.md` § "Sobre el multiplicador 3×".

This check can be implemented either as a periodic sweep (a background
thread that walks the registry every few seconds) or lazily (computed
on-the-fly whenever a QUERY/STATUS or QUERY/LIST_NODES needs to report
state). A periodic sweep is the more standard approach and is what to
implement — reporting stale data as "ACTIVE" until someone happens to query
it is a correctness gap a lazy-only approach leaves open.

## Client lifecycle

```
CONNECTED --AUTH--> (server calls PAM) --RESP(session_id)--> AUTHENTICATED
CONNECTED --AUTH (bad creds)--> ERROR(AUTH_FAILED), still CONNECTED
CONNECTED --3rd bad AUTH--> ERROR(AUTH_FAILED), connection closed
AUTHENTICATED --QUERY x N--> AUTHENTICATED
AUTHENTICATED --TCP connection drops--> session invalidated
```

1. Client opens a TCP connection and sends AUTH with a username/password.
2. Server authenticates against the OS's real user accounts via **PAM**
   (`pam_start`/`pam_authenticate`), not a file the project invents and
   maintains itself — this is what satisfies the "not a local-file/DIY user
   system" requirement.
3. On success: server generates a `session_id` (unique per active session,
   e.g. an incrementing counter or random value — doesn't need to be
   cryptographically strong for this project, but state that choice
   explicitly rather than silently picking something weak and hoping it
   doesn't come up), stores it against that connection, and replies RESP.
4. Client includes that `session_id` as `entity_id` on every subsequent
   QUERY. Server looks it up; if it doesn't match a live session, that's
   `SESSION_EXPIRED`.
5. On disconnect, the server discards the session — a new connection always
   needs a fresh AUTH.
6. A failed AUTH leaves the connection open so the client can retry a
   mistyped password, but the server counts failures per connection and
   closes the descriptor after the third, logging the fact. The counter is
   per connection, not per username — no state survives a disconnect.

## Concurrency model

Three kinds of concurrent activity touch the same server-side data (the
node registry: id, name, type, state, `last_telemetry_at`, and a
fixed-size ring buffer of recent telemetry readings per node):

- One thread per node TCP connection (handles that node's REGISTER, then
  blocks reading EVENT messages for the connection's lifetime).
- One thread per client TCP connection (handles AUTH, then blocks reading
  QUERY messages for the connection's lifetime).
- One dedicated thread reading the single shared UDP socket for all
  TELEMETRY from all nodes.

All three categories read and/or write the shared registry, so it needs a
mutex (a single global lock is simplest and fine to start with; a per-node
lock is a legitimate later optimization if lock contention actually shows
up under load — don't reach for it before measuring). Concretely: the UDP
thread locks to update a node's `last_telemetry_at` and push into its ring
buffer; a node's EVENT thread locks to update node state or append an
event; a client's QUERY thread locks to read whatever it's reporting on.
Keep the critical section (time spent actually holding the lock) as short
as possible — copy out what you need, then unlock, then do slower work
like formatting a RESP payload or writing to the log file outside the lock.

**Persistence tradeoff (explicit, not accidental):** the ring buffers live
in memory only. A server restart loses all telemetry history. That's an
acceptable simplification for a course project's scope — naming it here
means it can be defended as a conscious choice ("we prioritized concurrency
correctness and the required feature set over durability, and would add a
write-ahead log or embedded DB as the next step") rather than looking like
a gap nobody noticed.

## Error-handling matrix

| Situation                                                        | Response                          |
|---------------------------------------------------------------------|-----------------------------------|
| Header `version` not supported                                       | ERROR `VERSION_MISMATCH`, then close the connection (TCP) or drop (UDP) |
| `payload_length` doesn't match what's actually parseable for that `msg_type` | ERROR `MALFORMED_MSG`      |
| QUERY with a `session_id` the server doesn't recognize                | ERROR `SESSION_EXPIRED`            |
| EVENT with a `node_id` the server doesn't recognize (e.g. the server restarted and lost its in-memory registry, but the node doesn't know that yet) | ERROR `UNKNOWN_NODE` — this tells the node it must re-REGISTER before the server will accept anything else from it |
| TELEMETRY (UDP) with an unrecognized `node_id`                        | silently dropped, logged server-side — no reply is possible over UDP without inventing a reply channel the assignment doesn't ask for |
| AUTH with bad credentials                                            | ERROR `AUTH_FAILED`                |
| QUERY/STATUS or QUERY/HISTORY for a `target_node_id` that was never registered | ERROR `UNKNOWN_NODE`      |
| QUERY with an unrecognized `query_type`                              | ERROR `UNSUPPORTED_QUERY`          |
| Any other server-side failure (e.g. `malloc` failure, unexpected `errno`) | ERROR `INTERNAL_ERROR`, and this must still be logged with full detail server-side even though the client only sees a generic message |

Every request from a client or node either gets exactly one RESP or exactly
one ERROR (except TELEMETRY, which as UDP gets neither on success or
failure) — this "always exactly one reply on TCP" rule is worth stating
explicitly since it's what a QUERY-issuing client can rely on to know it's
safe to block on `recv()` for the answer.

## Assignment-requirement cross-checks

- **≥5 historical points**: satisfied by QUERY/HISTORY's `count` field —
  default should be 5, and the ring buffer's fixed size must be ≥5 per
  node so a HISTORY query can actually be answered in full.
- **No hardcoded IPs**: hostnames given on the command line are resolved
  via `getaddrinfo()`, and resolution failure (bad hostname, no DNS)
  produces a logged error and a backoff retry, never a crash and never
  process termination. Specified in `despliegue-y-nombres.md`.
- **Logging with the client's ip:port**: the exact line format, the source
  of the address field for TCP and UDP, and the thread-safety rules are
  specified in `logging.md`.
- **No DIY user auth**: satisfied by AUTH delegating to PAM against real
  system accounts (see Client lifecycle above). Note this requires the
  server process to have whatever PAM/shadow-read privileges the grading
  machine's PAM config demands — document the exact setup steps (which PAM
  service name to register, any required group membership) once that part
  is actually implemented, since "works on my machine" isn't good enough
  for something a professor will run themselves.
