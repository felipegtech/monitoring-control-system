# Design Documentation Index

Requirement 8 of the assignment (`assignment-brief.md`) recommends
structuring the protocol specification the way RFC 793 (TCP) does. This
index maps that exact structure to where each part lives, so it's traceable
during review or the oral defense instead of implicit.

| RFC-793-style section | Where it is |
|---|---|
| (a) Visión general del protocolo | `overview.md` |
| (b) Especificación del servicio (primitivas) | `service-spec.md` → "Service primitives" |
| (c) Formato de mensajes (encabezado) | `protocol.md` |
| (d) Reglas de procedimiento (flujo, máquinas de estado) | `service-spec.md` → everything after "Service primitives" |
| (e) Ejemplos de implementación (diagramas de secuencia) | not yet written — planned once the first end-to-end flow (REGISTER → TELEMETRY → EVENT → AUTH → QUERY) is finalized |

`transport-analysis.md` and `assignment-brief.md` support the above but
aren't part of the RFC structure itself: the first is the graded "análisis
preliminar sobre el uso de TCP y UDP" the brief asks for separately, the
second is the requirements/rubric these documents are built against.

## Reading order

1. `overview.md` — what problem this solves and the overall architecture.
2. `protocol.md` — the exact bytes on the wire.
3. `service-spec.md` — what the server does with those bytes, and when.
4. `transport-analysis.md` — why each message rides TCP or UDP.
