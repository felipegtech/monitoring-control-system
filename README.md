# Sistema de Monitoreo y Control Distribuido

Distributed monitoring and control system built for *Internet: Arquitectura
y Protocolos* (Telemática), EAFIT, 2026-2. A central server (pure C,
Berkeley sockets) receives telemetry and critical events from multiple
simulated sensor/IoT nodes, and answers status/history queries from one or
more authenticated admin clients. Nodes and clients never talk to each
other directly — everything is mediated by the server.

## Status

| Fase | Entrega | Estado |
|---|---|---|
| 1 — Diseño y arquitectura | 9 sept | ✅ propuesta inicial completa (ver `docs/`) |
| 2 — Comunicación básica | 23 sept | ⏳ no iniciada |
| 3 — Concurrencia, resiliencia y pruebas | 30 sept | ⏳ no iniciada |

## Repository layout

```
docs/
  README.md                     — index: RFC-793 structure, reading order, precedence
  assignment-brief.md           — verbatim copy of the graded requirements/rubric
  overview.md                   — problem description, architecture, entities
  protocol.md                   — wire format: header, msg_type enum, payloads, limits
  service-spec.md               — service primitives, lifecycles, concurrency, errors
  maquinas-de-estado.md         — the four state machines, as normative transition tables
  transport-analysis.md         — TCP vs UDP justification per message type
  sequence-diagrams.md          — the protocol in motion, end to end
  logging.md                    — console/file log format, ip:port identification
  despliegue-y-nombres.md       — getaddrinfo(), resolution failure, deployment scenarios
  trazabilidad-requerimientos.md — requirement → document map, deferred decisions
server/                         — central server source (C) — added in Fase 2
```

## Design documents

See `docs/README.md` for the reading order, how each document maps to the
RFC-793-style structure the assignment recommends, and which document wins
when two disagree. Short version: start with `docs/overview.md` for the big
picture, then `docs/protocol.md` for the exact bytes on the wire, then
`docs/service-spec.md` and `docs/maquinas-de-estado.md` for what the server
does with them. `docs/transport-analysis.md` justifies which messages ride
TCP vs UDP.

To check the design against the assignment rather than read it end to end,
start at `docs/trazabilidad-requerimientos.md` — it maps every numbered
requirement to the document that resolves it, and lists what is
deliberately deferred to Fase 2 and Fase 3.

## Building & running

Not yet available — the server implementation starts in Fase 2. It will be
built with `gcc` via a `Makefile` and run as:

```
./server <puerto> <archivoDeLogs>
```

## Team

- Central server (this component): implemented in C using only the
  Berkeley Sockets API.
- Node(s) and admin client(s): implemented by other team members, in
  whichever language they choose, communicating with the server per
  `docs/protocol.md`.

## Branching

`main` is kept in a working/gradeable state at every phase deadline, since
the graded evidence is the commit history itself, not a single final
upload. Once implementation starts (Fase 2), feature work happens on
short-lived `feature/<short-description>` branches merged back into `main`
once it builds and does what it claims — this keeps `main` bisectable if
something breaks later in Fase 3.

## Commits

The assignment is explicitly graded on the commit history showing
**progressive evolution** — "no se considerará suficiente la existencia de
una única carga final." One giant commit per phase reads as if the work
happened in one sitting; several small, meaningful commits read as what
actually happened. So: commit as you go, not just when a whole feature is
finished.

Format ([Conventional Commits](https://www.conventionalcommits.org/)):

```
<type>: <what changed, present tense, why if it's not obvious>
```

Types used in this repo: `feat` (new capability — e.g. a new message type
handled), `fix`, `docs`, `refactor`, `test`, `chore` (build/config/tooling).
Examples that fit this project:

```
feat(server): parse fixed 8-byte header before reading payload
feat(node): send REGISTER over TCP on startup
fix(server): correct off-by-one in payload_length bounds check
docs: add sequence diagram for the REGISTER/AUTH/QUERY flow
```

Keep the subject line under ~70 characters; add a short body below it if
the *why* isn't obvious from the *what*. Commit whatever compiles and does
something real — don't wait for a whole phase to be "done" to commit once.
