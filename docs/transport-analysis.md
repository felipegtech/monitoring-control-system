# Transport Layer Analysis

The assignment asks for this as its own analysis, evaluated against a
specific set of criteria, rather than left implicit in the message
definitions. Each message type is assessed below against all of them, and
the transport choice follows from that assessment — not the other way
around.

## Criteria applied to each message type

| Criterion | REGISTER | EVENT | TELEMETRY | AUTH / QUERY |
|---|---|---|---|---|
| Frecuencia de transmisión | Una sola vez por sesión del nodo | Rara (solo cuando ocurre un evento) | Alta (periódica, p. ej. cada pocos segundos) | Rara/moderada (una vez por sesión de cliente, luego una por consulta) |
| Tamaño del mensaje | Pequeño (nombre + tipo) | Pequeño-moderado (incluye texto libre) | Muy pequeño (una lectura) | Pequeño (credenciales o parámetros de consulta) |
| Criticidad de la información | Alta — sin esto el nodo no existe para el sistema | Alta — puede representar una falla real | Baja individualmente — el siguiente dato la reemplaza | Alta — controla el acceso al sistema |
| Tolerancia a pérdidas | Ninguna | Ninguna | Alta | Ninguna |
| Necesidad de orden | N/A (un solo mensaje) | Deseable pero no crítico (el timestamp del servidor resuelve el orden real) | No — cada lectura es independiente | Sí, dentro de una sesión (AUTH antes que cualquier QUERY) |
| Necesidad de confiabilidad | Total | Total | Ninguna a nivel de mensaje individual | Total |
| Necesidad de mantener una conexión | Sí — la conexión permanece abierta y es la señal de desconexión del nodo | Sí — reutiliza la conexión de REGISTER | No — no orientado a conexión | Sí — la sesión autenticada vive mientras la conexión TCP viva |
| Comportamiento ante retardo | Aceptable esperar la confirmación | Aceptable esperar la confirmación | Un retardo alto simplemente hace que el dato quede desactualizado hasta la siguiente lectura | Aceptable esperar la respuesta |
| Comportamiento ante pérdida de mensajes | Inaceptable — debe reintentarse a nivel de aplicación si la conexión falla antes de la respuesta | Inaceptable — un evento crítico perdido puede ocultar una falla real | Aceptable — se descarta y se espera la siguiente lectura | Inaceptable — una consulta perdida deja al cliente sin respuesta a lo que preguntó |

## Resulting decision

- **TCP (`SOCK_STREAM`)** for REGISTER, EVENT, AUTH, QUERY/RESP/ERROR: all
  of them require confirmed delivery, need the request/response pairing TCP
  gives for free, and (for REGISTER/EVENT) explicitly rely on TCP's
  connection state as the only reliable signal the server has that a node
  is still present — that signal literally doesn't exist in a connectionless
  protocol.
- **UDP (`SOCK_DGRAM`)** for TELEMETRY only: it's the one message type where
  every criterion points the same direction — frequent, tiny, individually
  low-value, no ordering requirement across readings, and actively better
  off *not* paying connection/retransmission overhead, since a delayed
  retransmit of an old reading is less useful than simply waiting for the
  next one.

## Why no application-layer reliability was added on top of UDP

The assignment explicitly allows (and expects, "cuando corresponda") adding
acknowledgment/retransmission/dedup/reordering on top of UDP if a message
type sent over UDP actually needs some of that reliability. TELEMETRY is
deliberately the *only* UDP message type, and it was chosen for UDP
precisely because it does **not** need any of those guarantees — adding an
ACK+retransmit scheme on top would just reimplement TCP's job for data that
was picked for UDP because it doesn't require it. If a future requirement
ever needed TELEMETRY to be loss-aware (e.g. detecting how many consecutive
readings were lost, for a "signal quality" metric), the sequence-number
mechanism to add would be: each TELEMETRY datagram carries a monotonically
increasing per-node sequence number, and the server tracks the last
sequence seen per node to detect gaps — noted here as a documented
non-requirement, not an oversight.
