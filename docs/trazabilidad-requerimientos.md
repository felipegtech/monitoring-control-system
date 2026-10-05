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
- **Implementado** — existe en el código (Fases 2 y 3) y está cubierto por
  `tests/test_server.py`. Dónde vive cada pieza en el código está en
  `implementacion.md`.

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
| a | Registrar los nodos que participan | `REGISTER` asigna `node_id` y crea entrada en el registro | Implementado |
| b | Recibir información periódica sobre su estado | `TELEMETRY` por UDP, búfer circular por nodo | Implementado |
| c | Recibir eventos generados por los nodos | `EVENT` por la conexión TCP del `REGISTER` | Implementado |
| d | Mantener información sobre el estado de los nodos | Registro en memoria con estados `REGISTERED`/`ACTIVE`/`STALE`/`DISCONNECTED` | Implementado |
| e | Responder consultas sobre la infraestructura | `QUERY` con tipos `STATUS`, `HISTORY`, `LIST_NODES` | Implementado |
| f | Atender múltiples clientes concurrentemente | Un hilo por conexión TCP + un hilo dedicado al socket UDP, registro protegido por mutex | Implementado |

---

## 3. Requerimientos transversales

| # | Requerimiento | Cómo se resuelve | Documento | Estado |
|---|---|---|---|---|
| 1 | Mínimo 2 nodos, 1 servidor, 1+ clientes | Arquitectura de tres entidades sin límite superior de nodos | `overview.md` | Diseñado |
| 2 | Sin comunicación directa nodo ↔ cliente | Ninguna primitiva del protocolo conecta esas dos entidades; el registro del servidor es el único punto de encuentro | `overview.md`, `service-spec.md` | Diseñado |
| 4 | Sin IPs codificadas; resolver nombres; manejar la excepción sin terminar | Nodos y clientes reciben un nombre de host y lo resuelven con `getaddrinfo()`; el fallo produce reintento con *backoff*, no terminación | `despliegue-y-nombres.md` | Implementado |
| 5 | Interfaz de cliente con estado instantáneo y ≥5 datos históricos | `QUERY/STATUS` y `QUERY/HISTORY` con `count` predeterminado en 5; el búfer circular por nodo tiene capacidad ≥5 | `protocol.md` § QUERY | Implementado |
| 6 | Autenticación y perfiles, sin sistema de usuarios propio en archivos locales | `AUTH` delega en PAM contra cuentas reales del sistema operativo | `service-spec.md` § Ciclo de vida del cliente | Implementado |
| 8 | Especificación suficiente para que un tercero la implemente | Estructura estilo RFC 793 repartida en los documentos de `docs/`, indexada en `docs/README.md` | `docs/README.md` | Diseñado |
| 9 | Integración con TCP/IP vía API de sockets Berkeley | `SOCK_STREAM` y `SOCK_DGRAM` sobre el mismo número de puerto, sin bibliotecas de terceros | `overview.md` § Arquitectura propuesta | Implementado |
| 10 | Soporte de múltiples clientes simultáneos (se permiten hilos) | Modelo hilo por conexión | `service-spec.md` § Modelo de concurrencia | Implementado |
| 11 | Control de excepciones | Matriz de errores con un código por situación, más las máquinas de estado que definen el comportamiento ante cada fallo | `service-spec.md` § Matriz de manejo de errores, `maquinas-de-estado.md` | Implementado |

---

## 4. Detalles de implementación exigidos

| Exigencia | Decisión | Documento | Estado |
|---|---|---|---|
| Servidor únicamente en C, solo API Berkeley | C11, `sys/socket.h`, `netdb.h`, `pthread.h`; sin bibliotecas de red de terceros | `overview.md` | Implementado |
| Clientes y nodos en cualquier lenguaje | Python 3 sin dependencias externas (`clients/`); el contrato es `protocol.md` | `protocol.md` | Implementado |
| Logging en consola y en archivo | Formato de línea único, idéntico en ambos destinos | `logging.md` | Implementado |
| Registro del identificador del cliente (ip y puerto origen) | Campo obligatorio en cada línea de log, obtenido de `accept()` o `recvfrom()` | `logging.md` | Implementado |
| Invocación `./server <puerto> <archivoDeLogs>` | Dos argumentos posicionales obligatorios | `logging.md` § Invocación | Implementado |
| Compilación con `gcc` y `Makefile` | `server/Makefile` (`make`, `make stub`, `make test`) | `implementacion.md` | Implementado |
| Servidor desplegable en nube (AWS Educate, IaaS) | Instancia EC2, puerto configurable, reglas de grupo de seguridad para TCP y UDP | `despliegue-y-nombres.md`, `implementacion.md` | Documentado |

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

## 6. Decisiones diferidas — cómo se resolvieron

Estas decisiones quedaron abiertas en la Fase 1. Se conserva la lista con
la resolución adoptada en la implementación.

| Tema | Resolución | Dónde |
|---|---|---|
| Conversión de enteros de 64 bits | Dos `htonl` sobre las mitades alta y baja (`wr_u64` / `rd_u64`) | `server/src/protocol.c` |
| Serialización de `float` | `memcpy` a `uint32_t` y luego `htonl`; en Python, `struct` con `"!f"` | `server/src/protocol.c`, `clients/mcsproto.py` |
| Serialización sin `memcpy` de structs | Lector y escritor acotados (`reader_t` / `writer_t`) que procesan campo por campo y detectan payloads truncados | `server/src/protocol.c` |
| Credenciales en texto claro | Se mantiene como limitación asumida; el servidor nunca escribe la contraseña en el log y borra su copia en memoria tras usarla | `server/src/conn.c` |
| Historial solo en memoria | Se mantiene como simplificación asumida | — |
| Purga de entradas `DISCONNECTED` | Automática a los 600 s por el barrido, o inmediata con `QUERY/PURGE_NODE` (perfil `ADMIN`) | `maquinas-de-estado.md` § 2 |
| Servicio PAM a registrar | Servicio `monitoreo`, instalado con `sudo make -C server install-pam`; configurable con `MCS_PAM_SERVICE` | `service-spec.md` § Perfiles |

## 7. Extensiones introducidas en la implementación

| Extensión | Motivo |
|---|---|
| `profile` (1B) en `RESP(AUTH)` | Gestión de perfiles del requerimiento 6 |
| `QUERY/EVENTS` (4) | Los eventos críticos se recibían pero ningún cliente podía consultarlos |
| `QUERY/PURGE_NODE` (5) | Operación de control que distingue a los perfiles; resuelve la purga diferida |
| Errores `NAME_IN_USE` (8), `PERMISSION_DENIED` (9), `INVALID_PARAM` (10) | No reutilizar `MALFORMED_MSG` para mensajes bien formados |
