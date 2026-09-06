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
| (d) Reglas de procedimiento (flujo, máquinas de estado) | `service-spec.md` (prosa) y `maquinas-de-estado.md` (tablas normativas) |
| (e) Ejemplos de implementación (diagramas de secuencia) | `sequence-diagrams.md` |

Supporting documents, outside the RFC structure itself:

| Documento | Qué cubre |
|---|---|
| `transport-analysis.md` | El "análisis preliminar sobre el uso de TCP y UDP" que la Fase 1 pide por separado |
| `despliegue-y-nombres.md` | Requerimiento 4: resolución de nombres, fallo de resolución, escenarios de despliegue |
| `logging.md` | Formato exacto del log en consola y archivo, con el identificador ip:puerto exigido |
| `trazabilidad-requerimientos.md` | Mapa requerimiento → documento, y las decisiones diferidas |
| `assignment-brief.md` | Copia literal de los requerimientos y la rúbrica |

## Reading order

1. `overview.md` — what problem this solves and the overall architecture.
2. `protocol.md` — the exact bytes on the wire.
3. `service-spec.md` — what the server does with those bytes, and when.
4. `maquinas-de-estado.md` — the same rules as normative state tables.
5. `transport-analysis.md` — why each message rides TCP or UDP.
6. `sequence-diagrams.md` — the above, illustrated end-to-end.

`despliegue-y-nombres.md` and `logging.md` are read when implementing, not
when learning the protocol. `trazabilidad-requerimientos.md` is the entry
point for review: it says which document answers each requirement.

## Normative precedence

Where two documents disagree, the more specific one wins:

1. `protocol.md` — byte layout, sizes, limits.
2. `maquinas-de-estado.md` — transitions, conditions, actions.
3. `service-spec.md` — everything else.

`service-spec.md` describes the same lifecycles in prose for readability;
`maquinas-de-estado.md` is what an implementer follows when the two are
phrased differently.
