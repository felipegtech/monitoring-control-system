# System Overview

## Problem description

An organization operates a distributed infrastructure made of heterogeneous
nodes — sensors, network equipment, IoT devices — that must be supervised
remotely. Each node needs to report two qualitatively different things to a
central point:

- **Periodic status** (telemetry: CPU usage, temperature, battery, uptime,
  operational metrics) — frequent, and an occasional lost reading doesn't
  matter because the next one supersedes it.
- **Critical events** (failures, threshold breaches, connectivity loss,
  alarms, major state changes) — rare, and losing one can have real
  consequences, so it can't be dropped silently the way a telemetry reading
  can.

A central server receives both kinds of information from many nodes at
once, keeps an up-to-date picture of the whole infrastructure, and answers
queries from one or more administration clients about that picture — both
the instantaneous state of a node and its recent history (at least 5 past
readings). Nodes and admin clients never talk to each other directly;
everything is mediated by the server, which must also authenticate clients
against real user accounts rather than a homegrown login system, and must
never hardcode an IP address (all endpoints are reached by resolving a
hostname).

## Proposed architecture

Three kinds of entities, one central mediator, no direct node↔client link:

```
 [Node 1]  [Node 2]  ...  [Node N]         [Admin Client 1] ... [Admin Client M]
     \        |             /                      |
      \       |            /                       |
       \      |           /                        |
        TCP (REGISTER,EVENT)   UDP (TELEMETRY)      TCP (AUTH, QUERY)
         \        \              /                 /
          \        \            /                 /
           \        \          /                 /
            v         v       v                 v
           +-------------------------------------+
           |          Central Server              |
           |  - one TCP listening socket           |
           |  - one UDP bound socket                |
           |  - shared node registry (mutex-guarded)|
           +-------------------------------------+
```

**One port, two sockets.** The server is invoked as `server <puerto>
<archivoDeLogs>` — a single port number. TCP and UDP have independent port
namespaces, so this one number is used to open *two* separate sockets: a
`SOCK_STREAM` socket that's `bind()`+`listen()`+`accept()`-ed for
REGISTER/EVENT/AUTH/QUERY, and a `SOCK_DGRAM` socket `bind()`-ed to the same
port number for TELEMETRY. They don't conflict because "port 5000/tcp" and
"port 5000/udp" are different addresses as far as the kernel's socket
tables are concerned — this is worth stating explicitly in the defense
since it's easy to assume one port means one socket.

**No hardcoded IPs.** Every participant (node, admin client) is configured
with a **hostname**, not an IP, for the server it connects to. Each
resolves it via `getaddrinfo()` immediately before connecting. If
resolution fails (typo, no DNS, network down), the participant logs the
failure and either retries after a backoff or exits with a clear error —
it must not crash (segfault, unhandled exception) as a result of a naming
failure. The server itself doesn't need to resolve anything outbound; it
only needs `getaddrinfo()`/`inet_ntop()` on the *receiving* side to log a
connecting client's address in a readable form.

## Participating entities

- **Node** — simulates a sensor/IoT device. Registers once (TCP), then
  concurrently: sends periodic TELEMETRY (UDP) and, when something
  noteworthy happens, sends an EVENT (TCP, same connection as REGISTER).
  Can be implemented in any language.
- **Central Server** — the component this repo/role owns. Pure C, Berkeley
  sockets only. Accepts many concurrent node and client TCP connections
  (thread-per-connection) plus a single UDP socket for all telemetry.
  Maintains the node registry (identity, state, recent readings) behind a
  mutex, authenticates admin clients via PAM against real system accounts,
  and logs every request/response (with the client's ip:port) to both
  console and the log file given on the command line.
- **Admin Client** — authenticates (AUTH), then issues QUERY messages to
  read a node's live status or its history (≥5 points) or to list known
  nodes. Can have a simple UI, in any language.
- **PAM / system accounts** — not a node or client in the protocol sense,
  but an explicit external dependency: the authentication authority the
  server delegates to, satisfying the "no DIY user system" requirement.
