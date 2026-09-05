# Sequence Diagrams

This is item (e) of the RFC-793-style structure from `docs/README.md` —
concrete examples of the protocol in motion, on top of the abstract
definitions in `protocol.md` and `service-spec.md`. GitHub renders these
Mermaid diagrams natively when viewing this file in the repo.

## 1. Node lifecycle — happy path

REGISTER once over TCP, then TELEMETRY repeatedly over UDP, with an EVENT
squeezed in whenever something noteworthy happens — all while the same TCP
connection from REGISTER stays open.

```mermaid
sequenceDiagram
    participant N as Node
    participant S as Server

    N->>S: TCP connect()
    N->>S: REGISTER(node_name, node_type, interval_hint)
    Note over S: entity_id=0 in header — node has no id yet
    S-->>N: RESP(in_response_to=REGISTER, assigned_node_id)
    Note over S: registry: node = REGISTERED

    loop every interval_hint seconds, over UDP
        N->>S: TELEMETRY(metric_type, value)
        Note over S: registry: node = ACTIVE, last_telemetry_at updated
    end

    N->>S: EVENT(event_code, severity, message)
    Note over N,S: same TCP connection as REGISTER
    S-->>N: RESP(in_response_to=EVENT)

    N--xS: TCP connection drops
    Note over S: registry: node = DISCONNECTED
```

## 2. Admin client — authenticate, then query

AUTH must succeed before any QUERY is accepted; the resulting `session_id`
travels in the header (`entity_id`) of every QUERY on that connection.

```mermaid
sequenceDiagram
    participant C as Admin Client
    participant S as Server

    C->>S: TCP connect()
    C->>S: AUTH(username, password)
    Note over S: server calls pam_authenticate()

    alt credentials valid
        S-->>C: RESP(in_response_to=AUTH, session_id)
        loop as many queries as needed
            C->>S: QUERY(query_type, target_node_id, count)
            Note over C,S: header entity_id = session_id
            S-->>C: RESP(in_response_to=QUERY, ...) or ERROR
        end
    else credentials invalid
        S-->>C: ERROR(in_response_to=AUTH, AUTH_FAILED)
    end
```

## 3. Error path — node reports after the server has forgotten it

Because history is in-memory only (see `service-spec.md`), a server
restart wipes the registry. A node that registered before the restart
doesn't know that yet, and finds out via an ERROR instead of a silent
failure.

```mermaid
sequenceDiagram
    participant N as Node (registered before restart)
    participant S as Server (just restarted, empty registry)

    N->>S: EVENT(event_code, severity, message)
    Note over N: entity_id = the node_id it was assigned before
    S-->>N: ERROR(in_response_to=EVENT, UNKNOWN_NODE)
    Note over N: must send REGISTER again before anything else is accepted
    N->>S: REGISTER(node_name, node_type, interval_hint)
    S-->>N: RESP(in_response_to=REGISTER, assigned_node_id)
```
