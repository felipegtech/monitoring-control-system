# Wire Protocol — Message Field Definitions

## Fixed header

Every message, regardless of transport (TCP or UDP), starts with this fixed
8-byte header:

```
[1B version][1B msg_type][4B entity_id][4B payload_length]
```

- `version` — protocol version (currently `1`). Lets the server reject a
  message from a client/node built against an incompatible future version
  instead of misparsing it.
- `msg_type` — see enum below.
- `entity_id` — **renamed from `node_id`**. It's not always a node: see
  "entity_id semantics" below.
- `payload_length` — number of bytes following the header for this message.
  The server does one `recv()` for these fixed 8 bytes, decodes
  `payload_length`, then does a second `recv()` (looped until it has that
  many bytes) for the payload.

All multi-byte integer fields, in the header and in every payload below, are
sent in **network byte order** (big-endian) and must be converted with
`htons`/`htonl` before sending and `ntohs`/`ntohl` after receiving.

## `msg_type` enum (revised from Fase 1 draft)

```
REGISTER = 1   // node -> server, TCP
EVENT    = 2   // node -> server, TCP
TELEMETRY= 3   // node -> server, UDP
QUERY    = 4   // client -> server, TCP
RESP     = 5   // server -> client or server -> node
ERROR    = 6   // server -> client or server -> node
AUTH     = 7   // client -> server, TCP  (NEW — see rationale below)
```

**Why `AUTH=7` is a revision, not an addition-only change:** the assignment
requires the server to authenticate admin clients against real system
accounts (PAM), not a homegrown file-based login. That's a real protocol
message — a client has to prove who it is before the server will answer a
QUERY — so it needs its own `msg_type` rather than being smuggled into an
existing one. AUTH *success/failure* reuse `RESP`/`ERROR` rather than adding
two more types, so the enum only grows by one.

## `entity_id` semantics

The same 4-byte header slot is reused for two different things depending on
who's talking, which is why it's no longer called `node_id`:

| Message                  | `entity_id` value                                  |
|---------------------------|----------------------------------------------------|
| REGISTER                  | `0` — node doesn't have an ID yet                   |
| EVENT, TELEMETRY          | the numeric node_id the server assigned at REGISTER |
| AUTH                      | `0` — client isn't authenticated yet                |
| QUERY                     | the `session_id` the server assigned at AUTH        |
| RESP / ERROR              | echoes whatever id makes sense for the reply (assigned node_id, session_id, or the requester's existing id) |

This means a client's very first message (AUTH) and a node's very first
message (REGISTER) both look like `entity_id=0` — the server tells them apart
purely by `msg_type`, never by the id field.

## Payloads — node-originated messages

These three are sent by a node, over the transport already fixed in Fase 1
(REGISTER/EVENT = TCP, TELEMETRY = UDP).

### REGISTER

Sent once, right after a node connects, `entity_id = 0` in the header
because the node has no numeric id yet.

| Field                     | Size     | Notes                                                        |
|----------------------------|----------|---------------------------------------------------------------|
| `node_name_len`             | 1B       | length in bytes of `node_name` that follows                  |
| `node_name`                 | variable | UTF-8, not null-terminated — length is carried explicitly, not inferred |
| `node_type`                  | 1B       | enum: `GENERIC=0, TEMPERATURE=1, HUMIDITY=2, MOTION=3, ...`   |
| `telemetry_interval_hint`    | 2B       | seconds; informational, used by the server for STALE timing (see service-spec.md) |

Why a length-prefixed string instead of a fixed-size `char name[32]`: a fixed
buffer either wastes space for short names or truncates long ones, and
reading a length-prefixed field is exactly the kind of "read N, then read
the next M bytes based on what N said" parsing you'll be doing by hand in C
for every variable-length field in this protocol — REGISTER is the simplest
place to learn that pattern before EVENT/RESP pile more of them on.

### EVENT

Sent over the same TCP connection the node registered on, whenever it has a
critical event to report. `entity_id` = the node's assigned id.

| Field            | Size     | Notes                                                                 |
|-------------------|----------|-------------------------------------------------------------------------|
| `event_code`        | 1B       | enum: `THRESHOLD_EXCEEDED=1, HARDWARE_FAULT=2, TAMPER=3, ...`          |
| `severity`           | 1B       | enum: `INFO=0, WARNING=1, CRITICAL=2`                                  |
| `node_timestamp`     | 8B       | node's own clock (Unix epoch seconds) — informational only, see below |
| `message_len`        | 2B       | length of `message` that follows                                      |
| `message`            | variable | human-readable free text, e.g. "temp 95C exceeds threshold 80C"       |

**Why the server doesn't trust `node_timestamp` for ordering:** nothing
guarantees a node's clock is correct or even set. The server stamps its own
receive time (`time()`/`clock_gettime()` at the moment `recv()` returns) for
anything that needs correct ordering (e.g. deciding which of two events
happened first), and only keeps `node_timestamp` around as context to show
an admin ("the node itself thought this happened at...").

### TELEMETRY

Sent over UDP, one reading per datagram, as often as the node's interval
dictates. `entity_id` = the node's assigned id.

| Field            | Size | Notes                                                    |
|-------------------|------|-----------------------------------------------------------|
| `metric_type`        | 1B   | enum matching the node's channel, e.g. `TEMPERATURE=1`     |
| `node_timestamp`     | 8B   | same caveat as EVENT — informational, not trusted for ordering |
| `value`              | 4B   | IEEE-754 float, the actual reading                         |

This is deliberately the smallest payload in the protocol — TELEMETRY is
frequent and loss-tolerant by design (a dropped UDP datagram just means the
server keeps the previous reading one interval longer), so there's no reason
to pay for anything beyond "which sensor, when, what value."

**One thing worth flagging for later, not solving now:** the server receives
UDP telemetry via `recvfrom()`, which gives it the sender's IP:port — but it
identifies *which node* sent it purely from the `entity_id` in the header,
not from the source address. That's intentional (a node's IP can change,
e.g. DHCP, without it needing to re-register), but it does mean the server
must silently drop any TELEMETRY whose `entity_id` isn't a currently
REGISTERED/ACTIVE node — there's no TCP connection to send an ERROR back on,
and UDP has no such thing as "rejecting" a datagram to its sender.

## Payloads — client-originated messages

Both sent over TCP by an admin client. `entity_id` starts at `0` (AUTH) and
becomes the assigned `session_id` (QUERY) — see the semantics table above.

### AUTH

Sent once, immediately after the client connects, before any QUERY is
allowed.

| Field            | Size     | Notes                                    |
|-------------------|----------|---------------------------------------------|
| `username_len`       | 1B       | length of `username`                      |
| `username`           | variable |                                             |
| `password_len`       | 1B       | length of `password`                      |
| `password`           | variable | sent in cleartext — see limitation below   |

**Named limitation, not an oversight:** this sends the password unencrypted
over TCP. The professional fix is TLS (wrap the socket, or at minimum hash +
salt + challenge-response instead of a raw password), which is out of scope
for a Berkeley-sockets-only course project. Say this explicitly in the oral
defense rather than hoping it doesn't come up — "we identified this and
scoped it out because X" reads far better than getting caught not having
noticed.

### QUERY

Sent any number of times over the same connection, once AUTH has succeeded.

| Field            | Size | Notes                                                                |
|-------------------|------|-------------------------------------------------------------------------|
| `query_type`         | 1B   | enum: `STATUS=1, HISTORY=2, LIST_NODES=3`                              |
| `target_node_id`     | 4B   | which node — meaningful for STATUS/HISTORY, ignored for LIST_NODES     |
| `count`              | 2B   | HISTORY only — how many recent points, e.g. default 5, capped at the server's ring-buffer size per node |

Three query types cover the assignment's requirements: live status of one
node (STATUS), the ≥5-point history requirement (HISTORY), and discovering
what nodes even exist to query (LIST_NODES) — an admin can't ask about a
node by id without first knowing which ids exist.

## Payloads — server replies (RESP, ERROR)

Both can be sent to either a node (answering REGISTER) or a client
(answering AUTH/QUERY). Because one `msg_type` now has to answer five
different request shapes, both payloads start with a 1-byte discriminator
saying *which* request they're answering, followed by a body whose shape
depends on that discriminator.

### RESP

| Field             | Size | Notes                                                     |
|--------------------|------|---------------------------------------------------------------|
| `in_response_to`      | 1B   | the `msg_type` being answered: `REGISTER`, `AUTH`, or `QUERY` |
| *(body — shape depends on `in_response_to`, and for QUERY, on which `query_type` the original request carried)* | | |

Body shapes:

- **answering REGISTER** → `assigned_node_id` (4B)
- **answering AUTH** → `session_id` (4B) — this value is *also* what the
  client must put in `entity_id` on every QUERY it sends afterward
- **answering QUERY/STATUS** → `node_id`(4B) + `state`(1B enum:
  `REGISTERED=0, ACTIVE=1, STALE=2, DISCONNECTED=3` — see service-spec.md's
  "Node lifecycle" for what each means and why there's no `UNKNOWN` value
  here) + `last_timestamp`(8B) + `last_value`(4B float)
- **answering QUERY/HISTORY** → `count`(2B) + `count` repetitions of
  {`timestamp`(8B), `value`(4B float)} — this is where the ≥5-point
  requirement actually shows up on the wire
- **answering QUERY/LIST_NODES** → `count`(2B) + `count` repetitions of
  {`node_id`(4B), `name_len`(1B), `name`(variable), `state`(1B)}

### ERROR

| Field           | Size     | Notes                                                              |
|------------------|----------|------------------------------------------------------------------------|
| `in_response_to`    | 1B       | the `msg_type` that failed                                          |
| `error_code`        | 1B       | see enum below                                                       |
| `message_len`       | 1B       | length of `message`                                                  |
| `message`           | variable | short human-readable string, e.g. for logging/debugging on the client end |

`error_code` enum:

```
AUTH_FAILED       = 1   // bad username/password on AUTH
UNKNOWN_NODE      = 2   // QUERY target_node_id doesn't exist
MALFORMED_MSG     = 3   // payload_length didn't match what was actually parseable
UNSUPPORTED_QUERY = 4   // query_type not recognized
SESSION_EXPIRED   = 5   // entity_id on a QUERY doesn't map to a live session
INTERNAL_ERROR    = 6   // server-side failure unrelated to the request itself
VERSION_MISMATCH  = 7   // header version field not supported
```

Note `UNKNOWN_NODE` only ever shows up as an ERROR in response to a
*client's* QUERY — a node's own EVENT/TELEMETRY referencing a bad state is
handled differently per transport (EVENT is TCP so it *could* get an ERROR
back too; TELEMETRY is UDP and is simply dropped, as noted above).
