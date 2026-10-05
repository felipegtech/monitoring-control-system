# Especificación de Logging

El enunciado exige que el servidor muestre en consola las peticiones
entrantes y las respuestas enviadas, que las almacene también en un archivo,
y que cada registro incluya el identificador del cliente —ip y puerto
origen de la petición—. Este documento define el formato exacto de esas
líneas, para que la implementación de la Fase 2 no tenga que improvisarlo y
para que un revisor pueda leer el archivo sin explicación previa.

## Invocación

```
./server <puerto> <archivoDeLogs>
```

Ambos argumentos son obligatorios y posicionales.

- `<puerto>` — número de puerto. Se usa para **dos** sockets: uno
  `SOCK_STREAM` y uno `SOCK_DGRAM`, según se explica en `overview.md`.
- `<archivoDeLogs>` — ruta del archivo. Se abre en modo *append*; si no
  existe se crea. Si no puede abrirse, el servidor escribe el error en
  `stderr` y termina con código distinto de cero, antes de abrir ningún
  socket. Arrancar sin poder registrar sería arrancar sin poder demostrar
  qué ocurrió.

Toda línea que se escribe en el archivo se escribe también, idéntica, en
`stdout`. No existen líneas que aparezcan solo en uno de los dos destinos.

## Formato de línea

Un solo formato para todos los eventos, de campos separados por espacio y
posición fija hasta el campo de detalle:

```
<timestamp> <nivel> <transporte> <ip>:<puerto> <dirección> <evento> [detalle]
```

| Campo | Descripción | Valores |
|---|---|---|
| `timestamp` | Marca de tiempo local con milisegundos, ISO 8601 | `2026-09-08T14:23:11.482-05:00` |
| `nivel` | Severidad | `INFO`, `WARN`, `ERROR` |
| `transporte` | Socket por el que ocurrió | `TCP`, `UDP`, `SYS` |
| `ip:puerto` | Identificador del cliente. Origen real del paquete, no lo que el mensaje declare | `192.168.1.42:51234`, o `-` para eventos del servidor |
| `dirección` | Sentido del mensaje | `RX` (recibido), `TX` (enviado), `--` (evento interno) |
| `evento` | Nombre del `msg_type`, o una etiqueta interna | `REGISTER`, `RESP`, `STATE_CHANGE`, … |
| `detalle` | Pares `clave=valor` separados por espacio. El contenido depende del evento | — |

### Origen del campo `ip:puerto`

- **TCP** — de la `struct sockaddr_in` que devuelve `accept()`, convertida
  con `inet_ntop()` y `ntohs()`. Se conserva durante toda la vida del hilo
  de conexión y se repite en cada línea de esa conexión.
- **UDP** — de la `struct sockaddr_in` que llena `recvfrom()`, por
  datagrama. En UDP no hay conexión, así que el puerto origen puede cambiar
  entre datagramas del mismo nodo; se registra el de cada datagrama tal
  como llegó.
- **SYS** — eventos que no provienen de ningún cliente (arranque, apagado,
  barrido de `STALE`). El campo se llena con `-`.

**El identificador del cliente nunca se deduce del `entity_id` del
mensaje.** Un `entity_id` es lo que el remitente afirma ser; la dirección
del socket es lo que la red observó. Ambos se registran, en campos
distintos, y esa separación es lo que permite detectar una suplantación.

## Ejemplos

Registro de un nodo y su confirmación:

```
2026-09-08T14:23:11.482-05:00 INFO TCP 192.168.1.42:51234 RX REGISTER entity_id=0 len=18 node_name="sensor-lab-01" node_type=1 interval=5
2026-09-08T14:23:11.483-05:00 INFO TCP 192.168.1.42:51234 TX RESP entity_id=7 len=5 in_response_to=REGISTER assigned_node_id=7
```

Telemetría por UDP (sin respuesta):

```
2026-09-08T14:23:16.501-05:00 INFO UDP 192.168.1.42:40912 RX TELEMETRY entity_id=7 len=13 metric_type=1 value=23.40
```

Autenticación de un cliente administrador:

```
2026-09-08T14:25:02.117-05:00 INFO TCP 10.0.0.8:44120 RX AUTH entity_id=0 len=22 username="dcuadros" password=<redacted>
2026-09-08T14:25:02.164-05:00 INFO TCP 10.0.0.8:44120 TX RESP entity_id=3001 len=6 in_response_to=AUTH session_id=3001 profile=ADMIN
```

Autenticación fallida:

```
2026-09-08T14:25:40.003-05:00 WARN TCP 10.0.0.8:44120 RX AUTH entity_id=0 len=20 username="admin" password=<redacted>
2026-09-08T14:25:40.051-05:00 WARN TCP 10.0.0.8:44120 TX ERROR entity_id=0 len=25 in_response_to=AUTH error_code=AUTH_FAILED attempt=1/3
```

Consulta y respuesta:

```
2026-09-08T14:26:11.900-05:00 INFO TCP 10.0.0.8:44120 RX QUERY entity_id=3001 len=7 query_type=HISTORY target_node_id=7 count=5
2026-09-08T14:26:11.902-05:00 INFO TCP 10.0.0.8:44120 TX RESP entity_id=3001 len=63 in_response_to=QUERY query_type=HISTORY returned=5
```

Mensaje mal formado:

```
2026-09-08T14:27:03.220-05:00 WARN TCP 10.0.0.8:44120 RX ??? entity_id=3001 len=99999 reason="payload_length excede MAX_PAYLOAD"
2026-09-08T14:27:03.221-05:00 WARN TCP 10.0.0.8:44120 TX ERROR entity_id=3001 len=30 in_response_to=0 error_code=MALFORMED_MSG
2026-09-08T14:27:03.221-05:00 INFO TCP 10.0.0.8:44120 -- CONN_CLOSE reason="malformed"
```

Datagrama UDP descartado (no genera respuesta, pero sí queda registrado):

```
2026-09-08T14:28:44.010-05:00 WARN UDP 192.168.1.99:35012 RX TELEMETRY entity_id=44 len=13 dropped=1 reason="entity_id sin entrada en el registro"
```

Eventos internos del servidor:

```
2026-09-08T14:20:00.000-05:00 INFO SYS - -- STARTUP tcp_port=5000 udp_port=5000 log="servidor.log" pid=8134
2026-09-08T14:29:31.775-05:00 WARN SYS - -- STATE_CHANGE node_id=7 from=ACTIVE to=STALE last_telemetry_age=16s
2026-09-08T14:31:02.400-05:00 INFO SYS - -- SHUTDOWN reason="SIGINT" open_conns=3
```

## Reglas

1. **Una línea por mensaje.** Cada `RX` tiene su `TX` correspondiente,
   salvo `TELEMETRY`, que por UDP no genera respuesta. Que la
   correspondencia sea visible en el archivo es lo que permite auditar el
   invariante de "exactamente una respuesta por petición sobre TCP".

2. **Las contraseñas nunca se escriben.** El campo `password` de `AUTH` se
   registra literalmente como `<redacted>`. El nombre de usuario sí se
   registra, porque es lo que permite investigar un intento de acceso
   fallido. Escribir la contraseña dejaría credenciales reales del sistema
   operativo en un archivo de texto plano de un proyecto de curso.

3. **El texto libre va entre comillas.** Los campos que pueden contener
   espacios —`node_name`, `message` de `EVENT`, `reason`— se encierran en
   comillas dobles. Un `"` dentro del texto se escapa como `\"`, y los
   saltos de línea se sustituyen por `\n` literal, para que una línea de
   log siga siendo una línea.

4. **El texto libre se trunca.** El campo `message` de `EVENT` puede llegar
   a 4084 bytes —el máximo que permite `MAX_PAYLOAD` una vez descontados
   los 12 bytes fijos del payload de `EVENT`; ver `protocol.md` § Límites
   de tamaño—. En el log se trunca a 200 caracteres seguidos de `…` para
   que un nodo no pueda hacer crecer el archivo sin control.

5. **La escritura es atómica respecto a los hilos.** Varios hilos escriben
   en el mismo archivo y en el mismo `stdout`. Cada línea se compone
   completa en un búfer local y luego se escribe dentro de una sección
   crítica protegida por un mutex propio del logger, distinto del mutex del
   registro de nodos. Componer la línea por partes dentro del bloqueo
   alargaría innecesariamente la sección crítica; escribirla sin bloqueo
   produciría líneas entremezcladas.

6. **El mutex del logger no se toma dentro del mutex del registro.** Se
   establece un orden fijo: si un hilo necesita ambos, toma primero el del
   registro, lo libera, y solo después escribe. Con un orden fijo no puede
   producirse un interbloqueo entre los dos.

7. **Se hace `fflush()` en cada línea.** Escribir con búfer haría que, ante
   una caída del servidor, se perdieran justamente las últimas líneas, que
   son las que explican la caída. El costo en rendimiento es irrelevante
   para la escala de este proyecto.

8. **Ningún fallo del logger detiene el servicio.** Si una escritura al
   archivo falla en tiempo de ejecución —disco lleno, archivo eliminado—,
   el servidor lo reporta en `stderr` y continúa atendiendo peticiones. La
   única falla de logging que impide arrancar es no poder abrir el archivo
   al inicio.

## Niveles

| Nivel | Cuándo se usa |
|---|---|
| `INFO` | Operación normal: mensajes válidos, respuestas exitosas, arranque y apagado |
| `WARN` | Situación anómala atribuible al cliente: mensaje mal formado, autenticación fallida, nodo desconocido, datagrama descartado, transición a `STALE` |
| `ERROR` | Fallo del servidor: `malloc` fallido, `errno` inesperado, error de PAM distinto de credenciales inválidas |

La distinción importa para la sustentación: `WARN` significa que el
protocolo funcionó —el servidor detectó la anomalía y respondió el código de
error correcto—, mientras que `ERROR` significa que algo del lado del
servidor no funcionó. Un archivo de log lleno de `WARN` durante una prueba
de robustez es evidencia de que el manejo de errores funciona, no de que
falle.
