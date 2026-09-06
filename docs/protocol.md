# Protocolo — Formato de Mensajes

## Encabezado fijo

Todo mensaje, sin importar el transporte (TCP o UDP), comienza con este
encabezado fijo de 10 bytes:

```
[1B version][1B msg_type][4B entity_id][4B payload_length]
```

- `version` — versión del protocolo (actualmente `1`). Permite que el
  servidor rechace un mensaje de un cliente o nodo construido contra una
  versión futura incompatible, en lugar de interpretarlo mal.
- `msg_type` — ver la enumeración más abajo.
- `entity_id` — **renombrado desde `node_id`**. No siempre es un nodo: ver
  la sección "Semántica de `entity_id`".
- `payload_length` — número de bytes que siguen al encabezado en este
  mensaje. El servidor lee estos 10 bytes fijos, decodifica
  `payload_length`, y luego lee exactamente esa cantidad de bytes para el
  payload.

Todos los campos enteros de más de un byte, tanto del encabezado como de
cualquier payload, se envían en **orden de red** (big-endian) y deben
convertirse con `htons`/`htonl` antes de enviar y con `ntohs`/`ntohl`
después de recibir.

Desplazamientos byte a byte, para que quien implemente no tenga que
contarlos:

| Desplazamiento | Tamaño | Campo |
|---|---|---|
| 0 | 1B | `version` |
| 1 | 1B | `msg_type` |
| 2 | 4B | `entity_id` |
| 6 | 4B | `payload_length` |
| **10** | — | primer byte del payload |

El encabezado son 10 bytes, no 8. Conviene afirmarlo explícitamente porque
`1 + 1 + 4 + 4` es fácil de sumar mal, y un analizador que suponga 8
desalinea todos los campos desde el primer mensaje en adelante.

## Límites de tamaño

| Constante | Valor | Qué acota |
|---|---|---|
| `HEADER_SIZE` | 10 bytes | Fijo, todo mensaje, ambos transportes |
| `MAX_PAYLOAD` | 4096 bytes | Máximo `payload_length` aceptado |

`payload_length` es un campo de 4 bytes, así que puede *declarar* hasta
4 GiB. El receptor debe rechazar cualquier valor superior a `MAX_PAYLOAD`
**antes de reservar memoria**, con `ERROR(MALFORMED_MSG)` y cierre de la
conexión; ver `maquinas-de-estado.md` § 4, regla 2. Sin esa comprobación,
un solo mensaje que declare un payload de 4 GiB agota la memoria del
servidor.

Consecuencias de `MAX_PAYLOAD` sobre los payloads individuales:

- **`message` de EVENT** — `message_len` es de 2B y podría declarar 65535,
  pero los 12 bytes fijos del payload dejan como máximo **4084** bytes
  utilizables. Una longitud declarada mayor es `MALFORMED_MSG`.
- **`count` de QUERY/HISTORY** — cada punto devuelto ocupa 12 bytes y la
  RESP lleva 3 bytes fijos, así que no caben más de 341 puntos en una
  respuesta. El búfer circular por nodo se dimensiona muy por debajo de
  ese límite (≥5 por requerimiento), de modo que el servidor simplemente
  acota `count` al tamaño del búfer.
- **QUERY/LIST_NODES** — con nombres de longitud variable, el servidor deja
  de agregar entradas en cuanto el payload superaría `MAX_PAYLOAD`, y
  reporta el `count` que efectivamente incluyó.

## Enumeración `msg_type`

```
REGISTER  = 1   // nodo -> servidor, TCP
EVENT     = 2   // nodo -> servidor, TCP
TELEMETRY = 3   // nodo -> servidor, UDP
QUERY     = 4   // cliente -> servidor, TCP
RESP      = 5   // servidor -> cliente, o servidor -> nodo
ERROR     = 6   // servidor -> cliente, o servidor -> nodo
AUTH      = 7   // cliente -> servidor, TCP
```

**Por qué `AUTH` tiene tipo propio:** el enunciado exige que el servidor
autentique a los clientes administradores contra cuentas reales del
sistema mediante PAM, no contra un login propio basado en archivos. Eso es
un mensaje de protocolo real —un cliente tiene que demostrar quién es antes
de que el servidor responda un QUERY— así que necesita su propio `msg_type`
en lugar de camuflarse dentro de otro. El éxito y el fallo de AUTH
reutilizan `RESP` y `ERROR` en vez de agregar dos tipos más, de modo que la
enumeración crece solo en uno.

## Semántica de `entity_id`

La misma ranura de 4 bytes del encabezado se reutiliza para dos cosas
distintas según quién esté hablando, y por eso ya no se llama `node_id`:

| Mensaje | Valor de `entity_id` |
|---|---|
| REGISTER | `0` — el nodo todavía no tiene identificador |
| EVENT, TELEMETRY | el `node_id` numérico que el servidor asignó en el REGISTER |
| AUTH | `0` — el cliente todavía no está autenticado |
| QUERY | el `session_id` que el servidor asignó en el AUTH |
| RESP / ERROR | devuelve el identificador que corresponda a la respuesta: el `node_id` asignado, el `session_id`, o el identificador que ya tenía quien preguntó |

Esto significa que el primer mensaje de un cliente (AUTH) y el primer
mensaje de un nodo (REGISTER) se ven ambos como `entity_id = 0`. El
servidor los distingue únicamente por el `msg_type`, nunca por el campo de
identificador.

## Payloads — mensajes originados en un nodo

Estos tres los envía un nodo, sobre el transporte ya fijado: REGISTER y
EVENT por TCP, TELEMETRY por UDP.

### REGISTER

Se envía una sola vez, justo después de que el nodo se conecta, con
`entity_id = 0` en el encabezado porque el nodo aún no tiene identificador
numérico.

| Campo | Tamaño | Notas |
|---|---|---|
| `node_name_len` | 1B | longitud en bytes del `node_name` que sigue |
| `node_name` | variable | UTF-8, sin terminador nulo: la longitud va explícita, no se infiere |
| `node_type` | 1B | enum: `GENERIC=0, TEMPERATURE=1, HUMIDITY=2, MOTION=3, ...` |
| `telemetry_interval_hint` | 2B | segundos; informativo, lo usa el servidor para el cálculo de `STALE` (ver `maquinas-de-estado.md`) |

Por qué una cadena con longitud al frente en lugar de un `char name[32]` de
tamaño fijo: un búfer fijo o desperdicia espacio con nombres cortos o trunca
los largos. Además, leer un campo con longitud al frente es exactamente el
patrón "lee N, luego lee los siguientes M bytes según lo que dijo N" que
este protocolo aplica en todos sus campos de longitud variable, y REGISTER
es el lugar más simple donde aparece.

### EVENT

Se envía sobre la misma conexión TCP en la que el nodo se registró, cada
vez que tiene un evento crítico que reportar. `entity_id` es el
identificador asignado al nodo.

| Campo | Tamaño | Notas |
|---|---|---|
| `event_code` | 1B | enum: `THRESHOLD_EXCEEDED=1, HARDWARE_FAULT=2, TAMPER=3, ...` |
| `severity` | 1B | enum: `INFO=0, WARNING=1, CRITICAL=2` |
| `node_timestamp` | 8B | reloj propio del nodo (época Unix, segundos) — solo informativo, ver abajo |
| `message_len` | 2B | longitud del `message` que sigue |
| `message` | variable | texto libre legible, p. ej. "temp 95C excede el umbral 80C" |

**Por qué el servidor no confía en `node_timestamp` para ordenar:** nada
garantiza que el reloj de un nodo esté correcto, ni siquiera configurado.
El servidor marca su propia hora de recepción (`time()` o
`clock_gettime()` en el instante en que retorna `recv()`) para todo lo que
requiera un orden correcto —por ejemplo, decidir cuál de dos eventos
ocurrió primero— y conserva `node_timestamp` únicamente como contexto para
mostrarle al administrador que el nodo creyó que aquello ocurrió en tal
momento.

### TELEMETRY

Se envía por UDP, una lectura por datagrama, con la frecuencia que dicte el
intervalo del nodo. `entity_id` es el identificador asignado al nodo.

| Campo | Tamaño | Notas |
|---|---|---|
| `metric_type` | 1B | enum correspondiente al canal del nodo, p. ej. `TEMPERATURE=1` |
| `node_timestamp` | 8B | misma salvedad que en EVENT: informativo, no se usa para ordenar |
| `value` | 4B | float IEEE-754, la lectura en sí |

El payload completo son 13 bytes. Es deliberadamente el más pequeño del
protocolo: TELEMETRY es frecuente y tolerante a pérdidas por diseño —un
datagrama UDP perdido solo significa que el servidor conserva la lectura
anterior un intervalo más— así que no hay razón para pagar por nada más
allá de "qué sensor, cuándo, qué valor".

**Identificación por `entity_id`, no por dirección de origen:** el servidor
recibe la telemetría con `recvfrom()`, que le entrega la ip:puerto del
remitente, pero determina *qué nodo* la envió únicamente a partir del
`entity_id` del encabezado. Es intencional, porque la IP de un nodo puede
cambiar —por DHCP o por NAT— sin que deba volver a registrarse. La
consecuencia es que el servidor debe descartar en silencio toda TELEMETRY
cuyo `entity_id` no corresponda a un nodo en estado `REGISTERED`, `ACTIVE`
o `STALE`: no hay conexión TCP por la cual devolver un ERROR, y en UDP no
existe la noción de "rechazar" un datagrama ante su remitente. El descarte
sí queda registrado en el log.

## Payloads — mensajes originados en un cliente

Ambos se envían por TCP desde un cliente administrador. `entity_id` empieza
en `0` (AUTH) y pasa a ser el `session_id` asignado (QUERY); ver la tabla
de semántica más arriba.

### AUTH

Se envía una sola vez, inmediatamente después de que el cliente se conecta
y antes de que se permita cualquier QUERY.

| Campo | Tamaño | Notas |
|---|---|---|
| `username_len` | 1B | longitud de `username` |
| `username` | variable | |
| `password_len` | 1B | longitud de `password` |
| `password` | variable | viaja en texto claro — ver la limitación abajo |

**Limitación declarada, no descuido:** esto envía la contraseña sin cifrar
sobre TCP. La solución profesional es TLS —envolver el socket, o como
mínimo sustituir la contraseña en claro por un esquema de reto-respuesta
con hash y sal— y queda fuera del alcance de un proyecto restringido a
sockets Berkeley. La limitación se asume y se documenta aquí de forma
explícita; ver también `trazabilidad-requerimientos.md` § 6.

### QUERY

Se envía cualquier número de veces sobre la misma conexión, una vez que el
AUTH tuvo éxito.

| Campo | Tamaño | Notas |
|---|---|---|
| `query_type` | 1B | enum: `STATUS=1, HISTORY=2, LIST_NODES=3` |
| `target_node_id` | 4B | a qué nodo se refiere — significativo para STATUS/HISTORY, ignorado para LIST_NODES |
| `count` | 2B | solo HISTORY — cuántos puntos recientes; el valor por omisión es 5 y se acota al tamaño del búfer circular por nodo |

Los tres tipos de consulta cubren los requerimientos del enunciado: el
estado instantáneo de un nodo (STATUS), el historial de al menos 5 puntos
(HISTORY), y el descubrimiento de qué nodos existen (LIST_NODES). Sin este
último, un administrador no puede preguntar por un nodo por identificador
sin saber antes qué identificadores existen.

## Payloads — respuestas del servidor (RESP, ERROR)

Ambas pueden dirigirse a un nodo (respondiendo REGISTER o EVENT) o a un
cliente (respondiendo AUTH o QUERY). Como un mismo `msg_type` tiene que
responder a cinco formas distintas de petición, los dos payloads empiezan
con un discriminador de 1 byte que indica *a cuál* petición responden,
seguido de un cuerpo cuya forma depende de ese discriminador.

### RESP

| Campo | Tamaño | Notas |
|---|---|---|
| `in_response_to` | 1B | el `msg_type` que se está respondiendo: `REGISTER`, `EVENT`, `AUTH` o `QUERY` |
| *(cuerpo — su forma depende de `in_response_to` y, para QUERY, del `query_type` que llevaba la petición original)* | | |

Formas del cuerpo:

- **respondiendo REGISTER** → `assigned_node_id` (4B)
- **respondiendo EVENT** → cuerpo vacío; la confirmación es la RESP misma
- **respondiendo AUTH** → `session_id` (4B). Este valor es *también* lo que
  el cliente debe poner en `entity_id` de cada QUERY que envíe después
- **respondiendo QUERY/STATUS** → `node_id` (4B) + `state` (1B, enum
  `REGISTERED=0, ACTIVE=1, STALE=2, DISCONNECTED=3`; ver
  `maquinas-de-estado.md` § 2 para qué significa cada uno y por qué no
  existe un valor `UNKNOWN`) + `last_timestamp` (8B) + `last_value` (4B
  float)
- **respondiendo QUERY/HISTORY** → `count` (2B) + `count` repeticiones de
  {`timestamp` (8B), `value` (4B float)}. Aquí es donde el requerimiento de
  los 5 puntos históricos aparece efectivamente en el cable
- **respondiendo QUERY/LIST_NODES** → `count` (2B) + `count` repeticiones
  de {`node_id` (4B), `name_len` (1B), `name` (variable), `state` (1B)}

### ERROR

| Campo | Tamaño | Notas |
|---|---|---|
| `in_response_to` | 1B | el `msg_type` que falló |
| `error_code` | 1B | ver la enumeración abajo |
| `message_len` | 1B | longitud de `message` |
| `message` | variable | cadena corta legible, útil para el log o la depuración del lado del cliente |

Enumeración de `error_code`:

```
AUTH_FAILED       = 1   // usuario o contraseña incorrectos en AUTH
UNKNOWN_NODE      = 2   // el node_id referido no existe en el registro
MALFORMED_MSG     = 3   // payload_length no corresponde a lo que se puede interpretar
UNSUPPORTED_QUERY = 4   // query_type no reconocido
SESSION_EXPIRED   = 5   // el entity_id de un QUERY no corresponde a una sesión viva
INTERNAL_ERROR    = 6   // fallo del servidor ajeno a la petición en sí
VERSION_MISMATCH  = 7   // campo version del encabezado no soportado
```

`UNKNOWN_NODE` aparece como respuesta al QUERY de un cliente y también al
EVENT de un nodo cuyo identificador el servidor ya no reconoce —por
ejemplo, tras un reinicio del servidor—. En ese segundo caso el ERROR le
indica al nodo que debe volver a registrarse antes de que se le acepte
nada más. En TELEMETRY no aplica: viaja por UDP y simplemente se descarta,
como se indicó arriba.
