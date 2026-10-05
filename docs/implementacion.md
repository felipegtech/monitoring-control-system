# Implementación (Fases 2 y 3)

Este documento conecta la especificación con el código: dónde vive cada
regla del protocolo, cómo se compila y se ejecuta todo, cómo se prueba y
cómo demostrar cada criterio de la rúbrica en la sustentación.

## Estructura

```
server/                      servidor central — C11, sockets Berkeley, pthreads, PAM
  Makefile                   make | make stub | make test | sudo make install-pam
  pam/monitoreo(.arch)       servicio PAM "monitoreo" (Debian/Ubuntu y Arch/Fedora)
  src/protocol.[ch]          constantes, enumeraciones, serialización campo por campo
  src/logger.[ch]            log en consola + archivo (formato de logging.md)
  src/registry.[ch]          registro de nodos, búferes circulares, máquina § 2
  src/auth.[ch]              PAM + perfil por grupo Unix
  src/conn.c                 hilo por conexión TCP: máquinas § 3 y § 4
  src/udp.c                  hilo del socket UDP de telemetría
  src/main.c                 argumentos, sockets, accept(), barrido, señales
clients/                     Python 3, solo biblioteca estándar
  mcsproto.py                el mismo protocolo, del lado de nodos y clientes
  node.py                    nodo sensor simulado (máquina § 1)
  admin.py                   cliente administrador: consultas y panel
tests/test_server.py         33 pruebas de integración por la red
```

## Mapa especificación → código

| Regla | Dónde |
|---|---|
| Encabezado de 10 bytes, orden de red | `header_encode` / `header_decode` en `server/src/protocol.c` |
| Payload truncado o con bytes sobrantes → `MALFORMED_MSG` | `reader_t` (`rd_*`, `rd_done`) en `protocol.c`; usado por cada `handle_*` de `conn.c` |
| Máquina de recepción TCP (§ 4): leer 10 bytes, validar versión y `MAX_PAYLOAD` antes de leer el payload | `conn_thread` en `server/src/conn.c` |
| `recv()` en bucle (fragmentación y mensajes pegados) | `recv_full` en `conn.c` |
| Exactamente una respuesta por mensaje TCP | `dispatch` → cada `handle_*` termina en `send_resp` o `send_error` |
| REGISTER / NAME_IN_USE / id nuevo tras DISCONNECTED | `handle_register` (`conn.c`) y `reg_register` (`registry.c`) |
| EVENT, UNKNOWN_NODE, INVALID_PARAM | `handle_event` (`conn.c`), `reg_event` |
| AUTH con PAM, 3 intentos, perfil | `handle_auth` (`conn.c`), `auth_check` (`auth.c`) |
| QUERY: sesión, tipos, permisos | `handle_query` y `query_*` (`conn.c`) |
| TELEMETRY por UDP, descartes, REGISTERED/STALE → ACTIVE | `udp_thread` (`udp.c`), `reg_telemetry` |
| ACTIVE → STALE, purga de DISCONNECTED | `sweeper_thread` (`main.c`), `reg_sweep` (`registry.c`) |
| → DISCONNECTED al cerrarse la conexión | final de `conn_thread`, `reg_disconnect` |
| Un mutex para el registro, otro para el log, nunca anidados | `reg_mtx` en `registry.c`; `log_mtx` en `logger.c` |
| Sockets TCP y UDP en el mismo puerto, dual-stack IPv4/IPv6 | `open_passive` en `main.c` |
| Resolución de nombres, recorrido de todas las direcciones, backoff | `connect_tcp` / `describe_gaierror` en `clients/mcsproto.py`; bucle `run` de `node.py` |
| EVENT con confirmación y reenvío tras reconexión | `flush_events` en `clients/node.py` |

## Compilación

Requisitos: `gcc`, `make`, cabeceras de PAM (`libpam0g-dev` en
Debian/Ubuntu, incluidas en `pam` en Arch) y Python 3.

```
make -C server            # ./server/server — binario real, autenticación PAM
make -C server stub       # ./server/server-stub — solo para pruebas (ver abajo)
make -C server test       # compila server-stub y corre las 33 pruebas
```

El código compila sin advertencias con `-Wall -Wextra -Wpedantic` y las
pruebas pasan también bajo AddressSanitizer, UndefinedBehaviorSanitizer y
ThreadSanitizer.

**`server-stub`** es el mismo servidor compilado con `-DMCS_AUTH_STUB`:
sustituye PAM por un verificador fijo (contraseña `test`; el usuario
`admin` recibe el perfil ADMIN) para que las pruebas automáticas no
dependan de cuentas reales. Al arrancar escribe `WARN ... AUTH_STUB` en el
log. **No debe usarse en la demostración ni en el despliegue.**

## Ejecución

### 1. Preparar la autenticación (una sola vez)

```
sudo make -C server install-pam       # crea /etc/pam.d/monitoreo
sudo groupadd mcs-admin               # perfil ADMIN
sudo usermod -aG mcs-admin <usuario>
sudo useradd -m operador && sudo passwd operador   # un usuario VIEWER para la demo
```

El servidor debe correr como root para que PAM pueda validar contraseñas
de usuarios distintos al que lo ejecuta. Sin root solo se puede iniciar
sesión con la cuenta propia (el log lo advierte con `PAM_PRIVILEGES`).

### 2. Nombre del servidor

Sin IPs en el código: en una sola máquina, agregar a `/etc/hosts`

```
127.0.0.1   monitoreo.local
```

### 3. Arrancar

```
sudo ./server/server 5000 servidor.log
python3 clients/node.py  monitoreo.local 5000 sensor-lab-01 2
python3 clients/node.py  monitoreo.local 5000 cpu-rack-02 3 --type 4 --base 50 --threshold 75
python3 clients/admin.py monitoreo.local 5000 -u <usuario>
```

En el cliente: `nodos`, `estado 1`, `historial 1`, `eventos 1`, `panel`,
`purgar <id>`, `ayuda`. En la terminal de un nodo se puede escribir
`falla`, `evento <texto>` o `salir` para generar eventos a demanda.

### 4. AWS (IaaS)

1. Instancia EC2 Ubuntu; `sudo apt install build-essential libpam0g-dev`.
2. Grupo de seguridad: **dos reglas de entrada** para el puerto elegido,
   una TCP y una UDP (sin la UDP, los nodos se quedan en `REGISTERED`).
3. Clonar, `make -C server`, `sudo make -C server install-pam`, crear el
   grupo y los usuarios como arriba.
4. `sudo ./server/server 5000 servidor.log` (o dentro de `tmux`).
5. Nodos y clientes usan el DNS público de la instancia
   (`ec2-…compute.amazonaws.com`), nunca la IP.

## Pruebas automatizadas

`tests/test_server.py` arranca `server-stub` en un puerto libre y lo
ejercita por la red. Cada caso exigido por la Fase 3 está cubierto:

| Situación de la Fase 3 | Pruebas |
|---|---|
| Mensajes con formato incorrecto | `test_oversized_payload_length_closes`, `test_inconsistent_payload_keeps_connection`, `test_query_with_wrong_length`, `test_invalid_msg_types_over_tcp`, `test_version_mismatch_closes`, `test_garbage_udp_is_dropped` |
| Parámetros inválidos | `test_event_invalid_severity`, `test_unknown_node_and_unsupported_query`, `test_purge_requires_admin_and_disconnected` |
| Desconexiones | `test_reregister_after_disconnect_gets_new_id`, `test_disconnect_mid_message`, `test_telemetry_from_disconnected_node_is_dropped` |
| Fallas de comunicación | `test_fragmented_and_coalesced_messages`, `test_idle_connection_without_first_message_is_harmless` |
| Nodos desconocidos | `test_event_before_register_and_with_wrong_id`, `test_garbage_udp_is_dropped` |
| Expiración de temporizadores | `test_stale_and_recovery` |
| Pérdida de mensajes | `test_stale_and_recovery` (telemetría que deja de llegar) |
| Autenticación y sesión | `test_three_failed_auth_close_connection`, `test_retry_after_failed_auth`, `test_query_before_auth_closes`, `test_wrong_session_id_keeps_connection`, `test_auth_profiles` |
| Concurrencia | `test_concurrent_nodes_and_clients` (15 nodos + 30 clientes a la vez) |
| Log | `test_log_has_peer_and_redacts_password` |
| Línea de comandos | `CommandLineTest` |

**Duplicación de mensajes.** Sobre TCP no hay duplicados en el transporte.
El único duplicado posible es de aplicación: un nodo reenvía un EVENT
cuya RESP se perdió porque la conexión cayó. El diseño elige entrega *al
menos una vez* para eventos críticos —perder una alarma es peor que verla
dos veces— y el administrador puede distinguir el duplicado porque ambos
llevan el mismo `node_ts`. TELEMETRY duplicada por la red no causa daño:
solo agrega una lectura al historial.

## Guion para la sustentación

| Criterio | Qué mostrar |
|---|---|
| API de sockets (5%) | `open_passive` en `main.c`: `getaddrinfo(AI_PASSIVE)` → `socket` → `setsockopt` → `bind` → `listen`; `accept()` en el bucle de `main` |
| Múltiples clientes (10%) | Dos nodos y dos clientes a la vez; `make -C server test` (la prueba de concurrencia); explicar por qué un hilo por conexión (`service-spec.md` § Modelo de concurrencia) |
| Lógica del protocolo (25%) | Recorrer `conn_thread` con `maquinas-de-estado.md` § 4 al lado; mostrar en el log cada RX con su TX |
| Conexión y petición (15%) | `clients/mcsproto.py`: `encode_*` contra las tablas de `protocol.md` |
| Manejo de respuestas (15%) | En `admin.py`: `estado 999` (UNKNOWN_NODE), `purgar` con un VIEWER (PERMISSION_DENIED), contraseña mala (AUTH_FAILED) |
| Robustez (10%) | Matar un nodo con Ctrl+C → DISCONNECTED; suspenderlo con Ctrl+Z → STALE y luego `fg` → ACTIVE; `node.py nombre.inexistente 5000 x 2` → EAI_NONAME y reintentos; `printf 'basura' \| nc monitoreo.local 5000` → el servidor sigue |
| Coordinación de mensajes (10%) | Un EVENT cuando el nodo pasa el umbral y su RESP inmediata; la transición REGISTERED → ACTIVE con la primera TELEMETRY |
| Especificación (10%) | `docs/README.md` y la estructura RFC 793 |

Preguntas probables, con la respuesta en el código o en los documentos:

- *¿Por qué el encabezado son 10 bytes y no un `struct` con `memcpy`?* —
  relleno del compilador; `protocol.c`.
- *¿Qué pasa si un `recv()` devuelve medio encabezado?* — `recv_full`
  acumula; prueba `test_fragmented_and_coalesced_messages`.
- *¿Por qué validar `payload_length` antes del `malloc`?* — un payload de
  4 GiB declarado agotaría la memoria; § 4 regla 2.
- *¿Cómo sabe el servidor que un nodo murió?* — cierre TCP → DISCONNECTED;
  silencio UDP → STALE; caída sin FIN → TCP keepalive.
- *¿Por qué no hay confirmación en TELEMETRY?* — `transport-analysis.md`.
- *¿Puede haber interbloqueo entre el mutex del registro y el del log?* —
  no, nunca se toman anidados (`logging.md` regla 6).
- *¿Por qué `localhost` funciona con IPv4 e IPv6?* — socket dual-stack con
  `IPV6_V6ONLY = 0`; `peer_to_str` muestra las IPv4 mapeadas como IPv4.
