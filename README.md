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
  assignment-brief.md    — verbatim copy of the graded requirements/rubric
  overview.md             — problem description, architecture, entities
  protocol.md              — wire format: header, msg_type enum, all payloads
  service-spec.md          — lifecycles, concurrency model, error handling
  transport-analysis.md    — TCP vs UDP justification per message type
server/                    — central server source (C) — added in Fase 2
```

## Design documents

Start with `docs/overview.md` for the big picture, then `docs/protocol.md`
for the exact bytes on the wire, then `docs/service-spec.md` for what the
server does with them. `docs/transport-analysis.md` justifies which
messages ride TCP vs UDP. `docs/assignment-brief.md` is the source of truth
for requirements this design is built against.

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
