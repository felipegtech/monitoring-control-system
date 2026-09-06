# Trazabilidad de Requerimientos

Este documento mapea cada requerimiento del enunciado
(`assignment-brief.md`) al lugar exacto del diseño donde queda resuelto. Su
propósito es que la revisión y la sustentación no dependan de buscar la
respuesta a través de nueve documentos.

Convención de estado:

- **Diseñado** — resuelto en la documentación de Fase 1.
- **Fase 2** / **Fase 3** — la decisión de diseño existe, la implementación
  corresponde a esa fase.
- **Pendiente** — falta decidirlo.

---

## 1. Mensajes mínimos del protocolo (requerimiento 7)

El enunciado exige siete operaciones mínimas. El protocolo las cubre con
siete valores de `msg_type`, sin ninguna operación exigida quedando sin
mensaje propio.

| # | Operación exigida | `msg_type` | Valor | Transporte | Definido en |
|---|---|---|---|---|---|
| a | Registro de un nodo | `REGISTER` | 1 | TCP | `protocol.md` § REGISTER |
| b | Envío de información periódica de estado | `TELEMETRY` | 3 | UDP | `protocol.md` § TELEMETRY |
| c | Envío de un evento | `EVENT` | 2 | TCP | `protocol.md` § EVENT |
| d | Confirmación o respuesta cuando sea necesaria | `RESP` | 5 | TCP | `protocol.md` § RESP |
| e | Consulta de información | `QUERY` | 4 | TCP | `protocol.md` § QUERY |
| f | Respuesta a una consulta | `RESP` | 5 | TCP | `protocol.md` § RESP, cuerpos de `QUERY/*` |
| g | Reporte de errores | `ERROR` | 6 | TCP | `protocol.md` § ERROR |

`AUTH` (valor 7) no corresponde a ninguna de las siete operaciones
mínimas: es un mensaje adicional que responde al requerimiento 6
(autenticación y perfiles de usuario).

`RESP` cubre los puntos (d) y (f) con un solo `msg_type` porque ambos son
la misma cosa desde el punto de vista del protocolo: la respuesta afirmativa
del servidor a una petición. Lo que distingue una confirmación de `REGISTER`
de una respuesta a `QUERY/HISTORY` es el discriminador `in_response_to` del
payload, no el tipo de mensaje. Definir dos `msg_type` separados obligaría a
duplicar la lógica de despacho sin ganar nada.

---

## 2. Obligaciones del servidor (requerimiento 3)

| # | Obligación | Cómo se resuelve | Estado |
|---|---|---|---|
| a | Registrar los nodos que participan | `REGISTER` asigna `node_id` y crea entrada en el registro | Diseñado — Fase 2 |
| b | Recibir información periódica sobre su estado | `TELEMETRY` por UDP, búfer circular por nodo | Diseñado — Fase 2 |
| c | Recibir eventos generados por los nodos | `EVENT` por la conexión TCP del `REGISTER` | Diseñado — Fase 2 |
| d | Mantener información sobre el estado de los nodos | Registro en memoria con estados `REGISTERED`/`ACTIVE`/`STALE`/`DISCONNECTED` | Diseñado — Fase 2 |
| e | Responder consultas sobre la infraestructura | `QUERY` con tipos `STATUS`, `HISTORY`, `LIST_NODES` | Diseñado — Fase 2 |
| f | Atender múltiples clientes concurrentemente | Un hilo por conexión TCP + un hilo dedicado al socket UDP, registro protegido por mutex | Diseñado — Fase 3 |

---

## 3. Requerimientos transversales

| # | Requerimiento | Cómo se resuelve | Documento | Estado |
|---|---|---|---|---|
| 1 | Mínimo 2 nodos, 1 servidor, 1+ clientes | Arquitectura de tres entidades sin límite superior de nodos | `overview.md` | Diseñado |
| 2 | Sin comunicación directa nodo ↔ cliente | Ninguna primitiva del protocolo conecta esas dos entidades; el registro del servidor es el único punto de encuentro | `overview.md`, `service-spec.md` | Diseñado |
| 4 | Sin IPs codificadas; resolver nombres; manejar la excepción sin terminar | Nodos y clientes reciben un nombre de host y lo resuelven con `getaddrinfo()`; el fallo produce reintento con *backoff*, no terminación | `despliegue-y-nombres.md` | Diseñado — Fase 2 |
| 5 | Interfaz de cliente con estado instantáneo y ≥5 datos históricos | `QUERY/STATUS` y `QUERY/HISTORY` con `count` predeterminado en 5; el búfer circular por nodo tiene capacidad ≥5 | `protocol.md` § QUERY | Diseñado — Fase 2 |
| 6 | Autenticación y perfiles, sin sistema de usuarios propio en archivos locales | `AUTH` delega en PAM contra cuentas reales del sistema operativo | `service-spec.md` § Ciclo de vida del cliente | Diseñado — Fase 3 |
| 8 | Especificación suficiente para que un tercero la implemente | Estructura estilo RFC 793 repartida en los documentos de `docs/`, indexada en `docs/README.md` | `docs/README.md` | Diseñado |
| 9 | Integración con TCP/IP vía API de sockets Berkeley | `SOCK_STREAM` y `SOCK_DGRAM` sobre el mismo número de puerto, sin bibliotecas de terceros | `overview.md` § Arquitectura propuesta | Diseñado — Fase 2 |
| 10 | Soporte de múltiples clientes simultáneos (se permiten hilos) | Modelo hilo por conexión | `service-spec.md` § Modelo de concurrencia | Diseñado — Fase 3 |
| 11 | Control de excepciones | Matriz de errores con un código por situación, más las máquinas de estado que definen el comportamiento ante cada fallo | `service-spec.md` § Matriz de manejo de errores, `maquinas-de-estado.md` | Diseñado — Fase 3 |

---

## 4. Detalles de implementación exigidos

| Exigencia | Decisión | Documento | Estado |
|---|---|---|---|
| Servidor únicamente en C, solo API Berkeley | C11, `sys/socket.h`, `netdb.h`, `pthread.h`; sin bibliotecas de red de terceros | `overview.md` | Fase 2 |
| Clientes y nodos en cualquier lenguaje | Definido por cada integrante; el contrato es `protocol.md` | `protocol.md` | Fase 2 |
| Logging en consola y en archivo | Formato de línea único, idéntico en ambos destinos | `logging.md` | Diseñado — Fase 2 |
| Registro del identificador del cliente (ip y puerto origen) | Campo obligatorio en cada línea de log, obtenido de `accept()` o `recvfrom()` | `logging.md` | Diseñado — Fase 2 |
| Invocación `./server <puerto> <archivoDeLogs>` | Dos argumentos posicionales obligatorios | `logging.md` § Invocación | Diseñado — Fase 2 |
| Compilación con `gcc` y `Makefile` | `Makefile` en la raíz de `server/` | — | Fase 2 |
| Servidor desplegable en nube (AWS Educate, IaaS) | Instancia EC2, puerto configurable, reglas de grupo de seguridad para TCP y UDP | `despliegue-y-nombres.md` | Fase 2 |

---

## 5. Entregables de la Fase 1

| Entregable exigido | Documento | Estado |
|---|---|---|
| Descripción general del problema | `overview.md` § Descripción del problema | Completo |
| Arquitectura propuesta | `overview.md` § Arquitectura propuesta | Completo |
| Entidades participantes | `overview.md` § Entidades participantes | Completo |
| Tipos de mensajes | `protocol.md` § Enumeración `msg_type` | Completo |
| Sintaxis preliminar | `protocol.md` § Payloads | Completo |
| Reglas básicas de comunicación | `service-spec.md` | Completo |
| Máquinas de estado | `maquinas-de-estado.md` | Completo |
| Análisis preliminar sobre el uso de TCP y UDP | `transport-analysis.md` | Completo |

Los documentos `sequence-diagrams.md`, `logging.md`,
`despliegue-y-nombres.md` y este mismo no son entregables exigidos por la
Fase 1, pero cubren el punto (e) de la estructura RFC 793 y adelantan
decisiones que la Fase 2 tendría que improvisar.

---

## 6. Decisiones diferidas

Las siguientes decisiones están identificadas pero no resueltas. Se listan
explícitamente para que consten como decisiones diferidas y no como
omisiones.

| Tema | Situación | Fase en que debe resolverse |
|---|---|---|
| Conversión de enteros de 64 bits | `node_timestamp` ocupa 8 bytes y no existe una función estándar equivalente a `htonl` para ese tamaño. Debe definirse una conversión manual byte a byte | Fase 2 |
| Serialización de `float` | El valor de `TELEMETRY` es IEEE-754 de 4 bytes. `htonl` no se le puede aplicar directamente; requiere copia a `uint32_t` con `memcpy` antes de convertir | Fase 2 |
| Serialización sin `memcpy` de structs | El relleno que el compilador inserta entre campos de 1, 2 y 4 bytes rompe el formato en el cable. La serialización debe ser campo por campo | Fase 2 |
| Credenciales en texto claro | `AUTH` transmite la contraseña sin cifrar. Limitación asumida y documentada; la solución sería TLS, fuera del alcance de un proyecto restringido a sockets Berkeley | Asumida |
| Historial solo en memoria | Un reinicio del servidor pierde el historial de telemetría. Simplificación asumida frente a añadir persistencia | Asumida |
| Purga de entradas `DISCONNECTED` | No está definido cuánto tiempo permanece en el registro un nodo desconectado antes de eliminarse | Fase 3 |
| Servicio PAM a registrar | No está definido el nombre del servicio PAM ni los permisos que el proceso necesita en la máquina de evaluación | Fase 3 |
