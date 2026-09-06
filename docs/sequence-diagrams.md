# Diagramas de Secuencia

Este documento es el punto (e) de la estructura estilo RFC 793 descrita en
`docs/README.md`: ejemplos concretos del protocolo en movimiento, sobre las
definiciones abstractas de `protocol.md` y `service-spec.md`. GitHub
renderiza estos diagramas Mermaid de forma nativa al ver el archivo en el
repositorio.

## 1. Ciclo de vida del nodo — camino feliz

Un REGISTER por TCP, luego TELEMETRY repetida por UDP, con algún EVENT
intercalado cuando ocurre algo relevante, todo mientras la misma conexión
TCP del REGISTER permanece abierta.

```mermaid
sequenceDiagram
    participant N as Nodo
    participant S as Servidor

    N->>S: TCP connect()
    N->>S: REGISTER(node_name, node_type, interval_hint)
    Note over S: entity_id=0 en el encabezado: el nodo aún no tiene id
    S-->>N: RESP(in_response_to=REGISTER, assigned_node_id)
    Note over S: registro: nodo = REGISTERED

    loop cada interval_hint segundos, por UDP
        N->>S: TELEMETRY(metric_type, value)
        Note over S: registro: nodo = ACTIVE, se actualiza last_telemetry_at
    end

    N->>S: EVENT(event_code, severity, message)
    Note over N,S: misma conexión TCP del REGISTER
    S-->>N: RESP(in_response_to=EVENT)

    N--xS: se cae la conexión TCP
    Note over S: registro: nodo = DISCONNECTED
```

## 2. Cliente administrador — autenticarse y luego consultar

El AUTH debe tener éxito antes de que se acepte cualquier QUERY; el
`session_id` resultante viaja en el encabezado (`entity_id`) de cada QUERY
de esa conexión.

```mermaid
sequenceDiagram
    participant C as Cliente Administrador
    participant S as Servidor

    C->>S: TCP connect()
    C->>S: AUTH(username, password)
    Note over S: el servidor invoca pam_authenticate()

    alt credenciales válidas
        S-->>C: RESP(in_response_to=AUTH, session_id)
        loop tantas consultas como se necesiten
            C->>S: QUERY(query_type, target_node_id, count)
            Note over C,S: entity_id del encabezado = session_id
            S-->>C: RESP(in_response_to=QUERY, ...) o ERROR
        end
    else credenciales inválidas
        S-->>C: ERROR(in_response_to=AUTH, AUTH_FAILED)
        Note over S: al tercer intento fallido se cierra la conexión
    end
```

## 3. Camino de error — el nodo reporta después de que el servidor lo olvidó

Como el historial vive solo en memoria (ver `service-spec.md`), un
reinicio del servidor borra el registro. Un nodo que se había registrado
antes del reinicio todavía no lo sabe, y se entera mediante un ERROR en
lugar de un fallo silencioso.

```mermaid
sequenceDiagram
    participant N as Nodo (registrado antes del reinicio)
    participant S as Servidor (recién reiniciado, registro vacío)

    N->>S: EVENT(event_code, severity, message)
    Note over N: entity_id = el node_id que le habían asignado antes
    S-->>N: ERROR(in_response_to=EVENT, UNKNOWN_NODE)
    Note over N: debe enviar REGISTER otra vez antes de que se le acepte nada
    N->>S: REGISTER(node_name, node_type, interval_hint)
    S-->>N: RESP(in_response_to=REGISTER, assigned_node_id)
    Note over S: se asigna un node_id nuevo, nunca el anterior
```

## 4. Transición a `STALE` — la telemetría se detiene sin que se caiga la conexión

Este es el escenario que justifica la existencia del estado `STALE` y, con
él, la elección de UDP para la telemetría. La conexión TCP sigue viva, así
que el servidor no puede declarar al nodo `DISCONNECTED`, pero los
datagramas dejaron de llegar.

```mermaid
sequenceDiagram
    participant N as Nodo
    participant S as Servidor
    participant B as Barrido periódico

    Note over N,S: el nodo está ACTIVE, interval_hint = 5 s

    N->>S: TELEMETRY (UDP)
    Note over S: last_telemetry_at = t0

    Note over N,S: los datagramas siguientes se pierden en la red

    B->>S: recorre el registro
    Note over S: ahora - t0 > 3 x 5 s
    Note over S: registro: nodo = STALE, se escribe en el log

    N->>S: TELEMETRY (UDP) — se restablece la red
    Note over S: registro: nodo = ACTIVE otra vez, se escribe la transición
```

Obsérvese que el nodo nunca se enteró de nada: desde su punto de vista
siguió en `ACTIVO` todo el tiempo, enviando datagramas. Esa divergencia
entre las dos vistas está explicada en `maquinas-de-estado.md` § 5.
