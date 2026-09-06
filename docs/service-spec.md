# Especificación del Servicio

Documento complementario de `protocol.md`. Aquel define *qué bytes* lleva
cada mensaje; este define *cuándo* se envían, *qué hace el servidor* con
ellos y *cómo se mantiene seguro el acceso concurrente* al estado
compartido.

## Primitivas del servicio

Antes del formato a nivel de bytes conviene enunciar el servicio que ofrece
el protocolo con independencia de su codificación: las operaciones que un
*usuario* del protocolo —un nodo o un cliente administrador— puede invocar,
de la misma forma en que un RFC describe primitivas como `SEND` y `RECEIVE`
antes de entrar en el formato de los paquetes.

| Primitiva | La invoca | Efecto | Respuesta |
|---|---|---|---|
| `REGISTER(node_name, node_type, interval_hint)` | Nodo | Registra el nodo en el registro del servidor y obtiene una identidad numérica | `RESP(assigned_node_id)` o `ERROR` |
| `EVENT(event_code, severity, message)` | Nodo | Reporta un suceso crítico y puntual asociado a un nodo ya registrado | `RESP` (confirmación) o `ERROR` |
| `TELEMETRY(metric_type, value)` | Nodo | Reporta una lectura periódica | ninguna — deliberadamente sin confirmación (ver `transport-analysis.md`) |
| `AUTH(username, password)` | Cliente administrador | Autentica al cliente contra cuentas reales del sistema operativo mediante PAM | `RESP(session_id)` o `ERROR(AUTH_FAILED)` |
| `QUERY(query_type, target_node_id, count)` | Cliente administrador | Solicita STATUS, HISTORY o LIST_NODES | `RESP(...)` o `ERROR` |

Toda primitiva salvo `TELEMETRY` es **confirmada**: quien la invoca recibe
siempre exactamente un `RESP` o un `ERROR`, nunca silencio. Esa es una
propiedad del *servicio* en sí, no una casualidad de la implementación, y
es la razón por la que REGISTER, EVENT, AUTH y QUERY están sobre TCP: una
primitiva que promete respuesta necesita un transporte con un lugar natural
donde esperarla. `TELEMETRY` es la única primitiva que intencionalmente no
hace esa promesa, y es exactamente por eso la única sobre UDP.

## Ciclo de vida del nodo

Hay dos máquinas de estado en juego, desde dos puntos de vista distintos.
Conviene no confundirlas, porque usan palabras que se solapan para
significar cosas diferentes.

**La vista que el nodo tiene de sí mismo** (interna a la implementación del
nodo, nunca se transmite como campo):

```
DESCONECTADO --envía REGISTER--> REGISTRANDO --recibe RESP--> ACTIVO
ACTIVO --se cae la conexión TCP--> DESCONECTADO
```

**La vista que el servidor lleva de ese nodo** (esto es lo que
efectivamente se almacena en el registro y se reporta en QUERY/STATUS;
corresponde a la enumeración de estados de `protocol.md`):

```
(sin entrada — se reporta como ERROR UNKNOWN_NODE, no como estado) --REGISTER exitoso--> REGISTERED
REGISTERED --llega la primera TELEMETRY--> ACTIVE
ACTIVE --sin TELEMETRY dentro del umbral--> STALE
STALE --se reanuda la TELEMETRY--> ACTIVE
REGISTERED, ACTIVE o STALE --se cae la conexión TCP--> DISCONNECTED
```

**Nota sobre `DISCONNECTED`:** el primer bosquejo de este diseño describía
la cadena del servidor como `UNKNOWN → REGISTERED → ACTIVE → STALE`, sin un
`DISCONNECTED` explícito. Agregarlo es una extensión deliberada y
declarada: sin él, un nodo cuya conexión TCP se cae quedaría reportado como
`REGISTERED`, `ACTIVE` o `STALE` para siempre. `UNKNOWN`, en cambio, no
necesita código de estado alguno: un identificador que el servidor nunca ha
visto simplemente no tiene entrada en el registro, que es exactamente lo
que ya cubre el código de error `UNKNOWN_NODE`.

La versión normativa de ambas máquinas, con tabla explícita de
evento / condición / acción por transición, está en `maquinas-de-estado.md`
§ 1 y § 2. Los bosquejos de arriba son el resumen; ese documento es lo que
sigue quien implementa.

Secuencia de sucesos:

1. El nodo abre una conexión TCP con el servidor y envía REGISTER con
   `entity_id = 0`.
2. El servidor comprueba que el `node_name` solicitado no esté en uso por
   otro nodo *actualmente conectado*, asigna un `node_id` numérico nuevo,
   almacena el nodo en su registro como `REGISTERED` y responde `RESP` con
   el identificador asignado.
3. El nodo mantiene esa conexión TCP abierta durante el resto de su vida:
   es a la vez el canal para futuros mensajes EVENT y aquello cuyo cierre
   es la forma en que el servidor llega a enterarse de que el nodo ya no
   está. Esta es *la razón* por la que REGISTER y EVENT van sobre TCP: el
   estado de conexión de TCP es lo que hace posible detectar la
   desconexión. UDP no tiene noción de "el cliente colgó".
4. El nodo comienza a enviar TELEMETRY por UDP, de forma independiente de
   la conexión TCP, usando su `node_id` asignado como `entity_id`. En el
   momento en que llega el *primer* datagrama de telemetría, el servidor
   promueve el nodo de `REGISTERED` a `ACTIVE`. `REGISTERED` es
   específicamente la ventana —normalmente breve— en la que un nodo
   completó el registro pero todavía no ha demostrado que su canal de
   telemetría funcione.
5. Si la conexión TCP se cierra —caída del nodo, corte de red, apagado
   ordenado— el `recv()` del servidor sobre esa conexión retorna 0 o error,
   y el nodo pasa a `DISCONNECTED` sin importar en cuál de
   `REGISTERED`/`ACTIVE`/`STALE` estuviera. El servidor debe entonces dejar
   de confiar en cualquier TELEMETRY que siga llegando con ese `node_id`
   hasta que el nodo vuelva a registrarse: un identificador abandonado
   podría permitir que un proceso *distinto* suplantara la telemetría de un
   nodo que ya no está realmente ahí.

## Detección de `STALE`

Un nodo puede tener su conexión TCP perfectamente viva —así que no está
`DISCONNECTED`— mientras su telemetría deja de llegar en silencio. `STALE`
existe porque TELEMETRY va sobre UDP y por naturaleza no puede avisar
cuándo se detiene: no hay FIN, no hay RST, no hay nada. El servidor tiene
que notar por sí mismo la *ausencia* de paquetes.

Mecanismo: cada registro de nodo almacena `last_telemetry_at` —la marca de
tiempo de recepción del propio servidor, no la del nodo— y
`telemetry_interval_hint`, tomado del REGISTER. Un nodo se considera
`STALE` cuando `ahora - last_telemetry_at > 3 × telemetry_interval_hint`.
Por qué el multiplicador es 3 y no 2 o 5, y qué sustituye el servidor
cuando `interval_hint` llega en `0`, está especificado en
`maquinas-de-estado.md` § "Sobre el multiplicador 3×".

La comprobación se implementa como un **barrido periódico**: un hilo en
segundo plano que recorre el registro cada pocos segundos. La alternativa
—calcularlo de forma perezosa solo cuando un QUERY/STATUS o un
QUERY/LIST_NODES necesita reportar el estado— deja abierta una brecha de
corrección: reportaría como `ACTIVE` un nodo cuya telemetría se detuvo,
hasta que alguien casualmente lo consulte.

## Ciclo de vida del cliente

```
CONECTADO --AUTH--> (el servidor invoca PAM) --RESP(session_id)--> AUTENTICADO
CONECTADO --AUTH con credenciales inválidas--> ERROR(AUTH_FAILED), sigue CONECTADO
CONECTADO --tercer AUTH fallido--> ERROR(AUTH_FAILED), se cierra la conexión
AUTENTICADO --QUERY x N--> AUTENTICADO
AUTENTICADO --se cae la conexión TCP--> la sesión se invalida
```

1. El cliente abre una conexión TCP y envía AUTH con usuario y contraseña.
2. El servidor autentica contra las cuentas reales de usuario del sistema
   operativo mediante **PAM** (`pam_start` / `pam_authenticate`), no contra
   un archivo que el proyecto invente y mantenga por su cuenta. Esto es lo
   que satisface el requerimiento de no usar un sistema de usuarios propio
   basado en archivos locales.
3. Si tiene éxito, el servidor genera un `session_id` único por sesión
   activa —un contador incremental o un valor aleatorio; no necesita ser
   criptográficamente fuerte para este proyecto, y esa decisión se declara
   aquí de forma explícita en vez de quedar implícita—, lo asocia a esa
   conexión y responde `RESP`.
4. El cliente incluye ese `session_id` como `entity_id` en cada QUERY
   posterior. El servidor lo busca; si no corresponde a una sesión viva, la
   respuesta es `SESSION_EXPIRED`.
5. Al desconectarse, el servidor descarta la sesión. Una conexión nueva
   siempre requiere un AUTH nuevo.
6. Un AUTH fallido deja la conexión abierta para que el cliente pueda
   reintentar una contraseña mal escrita, pero el servidor cuenta los
   fallos por conexión y cierra el descriptor tras el tercero, dejándolo
   registrado en el log. El contador es por conexión, no por usuario: no
   sobrevive ningún estado a una desconexión.

## Modelo de concurrencia

Tres clases de actividad concurrente tocan los mismos datos del servidor
—el registro de nodos: identificador, nombre, tipo, estado,
`last_telemetry_at` y un búfer circular de tamaño fijo con las lecturas
recientes de cada nodo—:

- Un hilo por conexión TCP de nodo: atiende el REGISTER de ese nodo y
  después se bloquea leyendo mensajes EVENT durante toda la vida de la
  conexión.
- Un hilo por conexión TCP de cliente: atiende el AUTH y después se bloquea
  leyendo mensajes QUERY durante toda la vida de la conexión.
- Un hilo dedicado que lee el único socket UDP compartido, por el que llega
  toda la telemetría de todos los nodos.

Las tres categorías leen o escriben el registro compartido, así que
necesita un mutex. Un único candado global es lo más simple y suficiente
para la escala de este proyecto; un candado por nodo es una optimización
posterior legítima si la contención llega a manifestarse bajo carga. En
concreto: el hilo UDP toma el candado para actualizar el
`last_telemetry_at` de un nodo e insertar en su búfer circular; el hilo
EVENT de un nodo lo toma para actualizar el estado o agregar un evento; el
hilo QUERY de un cliente lo toma para leer aquello que va a reportar.

La sección crítica —el tiempo que efectivamente se sostiene el candado— se
mantiene lo más corta posible: se copia hacia afuera lo que se necesita, se
libera el candado, y solo después se hace el trabajo lento, como componer
el payload de una RESP o escribir en el archivo de log. El orden de toma de
candados frente al mutex del logger está fijado en `logging.md`, regla 6.

**Compromiso de persistencia, explícito y no accidental:** los búferes
circulares viven solo en memoria. Un reinicio del servidor pierde todo el
historial de telemetría. Es una simplificación aceptable para el alcance de
este proyecto, y se declara aquí como decisión consciente: se priorizó la
corrección de la concurrencia y el conjunto de funcionalidades exigido por
encima de la durabilidad. El siguiente paso natural sería un registro de
escritura anticipada o una base de datos embebida.

## Matriz de manejo de errores

| Situación | Respuesta |
|---|---|
| Campo `version` del encabezado no soportado | `ERROR(VERSION_MISMATCH)` y cierre de la conexión (TCP), o descarte (UDP) |
| `payload_length` no corresponde a lo interpretable para ese `msg_type` | `ERROR(MALFORMED_MSG)` |
| `payload_length` mayor que `MAX_PAYLOAD` | `ERROR(MALFORMED_MSG)` y cierre de la conexión, sin reservar memoria |
| QUERY con un `session_id` que el servidor no reconoce | `ERROR(SESSION_EXPIRED)` |
| QUERY antes de un AUTH exitoso | `ERROR(SESSION_EXPIRED)` y cierre de la conexión |
| EVENT con un `node_id` que el servidor no reconoce (p. ej. el servidor se reinició y perdió su registro en memoria, pero el nodo todavía no lo sabe) | `ERROR(UNKNOWN_NODE)`, que le indica al nodo que debe volver a registrarse antes de que se le acepte nada más |
| TELEMETRY (UDP) con un `node_id` no reconocido | Se descarta en silencio y se registra en el log. No hay respuesta posible sobre UDP sin inventar un canal de retorno que el enunciado no pide |
| AUTH con credenciales incorrectas | `ERROR(AUTH_FAILED)`; al tercer intento fallido se cierra la conexión |
| QUERY/STATUS o QUERY/HISTORY sobre un `target_node_id` nunca registrado | `ERROR(UNKNOWN_NODE)` |
| QUERY con un `query_type` no reconocido | `ERROR(UNSUPPORTED_QUERY)` |
| Cualquier otro fallo del lado del servidor (p. ej. `malloc` fallido, `errno` inesperado) | `ERROR(INTERNAL_ERROR)`. Debe quedar registrado con todo el detalle del lado del servidor, aunque el cliente solo vea un mensaje genérico |

Toda petición de un cliente o de un nodo recibe exactamente un `RESP` o
exactamente un `ERROR`, salvo TELEMETRY, que al ir sobre UDP no recibe
ninguno ni en éxito ni en fallo. Esta regla de "siempre exactamente una
respuesta sobre TCP" se enuncia de forma explícita porque es aquello en lo
que un cliente que emite un QUERY se apoya para saber que puede bloquearse
en `recv()` esperando la respuesta.

## Comprobaciones frente a los requerimientos

- **Al menos 5 puntos históricos**: lo satisface el campo `count` de
  QUERY/HISTORY, con valor por omisión 5, y el tamaño fijo del búfer
  circular, que debe ser ≥5 por nodo para que una consulta de historial
  pueda responderse completa.
- **Sin IPs codificadas**: los nombres de host se reciben por línea de
  comandos y se resuelven con `getaddrinfo()`. El fallo de resolución
  —nombre inexistente, sin DNS— produce un error registrado y un reintento
  con *backoff*, nunca una caída ni la terminación del proceso.
  Especificado en `despliegue-y-nombres.md`.
- **Logging con la ip:puerto del cliente**: el formato exacto de línea, el
  origen del campo de dirección para TCP y para UDP, y las reglas de
  seguridad entre hilos están especificados en `logging.md`.
- **Sin sistema de usuarios propio**: lo satisface AUTH delegando en PAM
  contra cuentas reales del sistema (ver el ciclo de vida del cliente más
  arriba). Esto exige que el proceso del servidor tenga los privilegios de
  PAM que requiera la configuración de la máquina donde se evalúe. El
  nombre del servicio PAM a registrar y los permisos concretos están
  listados como decisión diferida en `trazabilidad-requerimientos.md` § 6.
