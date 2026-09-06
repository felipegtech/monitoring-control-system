# Sistema de Monitoreo y Control Distribuido

Sistema de monitoreo y control distribuido construido para *Internet:
Arquitectura y Protocolos* (Telemática), EAFIT, 2026-2. Un servidor central
—C puro, sockets Berkeley— recibe telemetría y eventos críticos de varios
nodos sensores/IoT simulados, y responde consultas de estado e historial de
uno o más clientes administradores autenticados. Los nodos y los clientes
nunca se comunican entre sí directamente: todo está mediado por el
servidor.

## Integrantes

- David Cuadros
- Felipe Gómez

## Estado

| Fase | Entrega | Estado |
|---|---|---|
| 1 — Diseño y arquitectura | 9 sept | ✅ propuesta inicial completa (ver `docs/`) |
| 2 — Comunicación básica | 23 sept | ⏳ no iniciada |
| 3 — Concurrencia, resiliencia y pruebas | 30 sept | ⏳ no iniciada |

## Estructura del repositorio

```
docs/
  README.md                      — índice: estructura RFC 793, orden de lectura, precedencia
  assignment-brief.md            — copia literal de los requerimientos y la rúbrica
  overview.md                    — descripción del problema, arquitectura, entidades
  protocol.md                    — formato de mensajes: encabezado, msg_type, payloads, límites
  service-spec.md                — primitivas del servicio, ciclos de vida, concurrencia, errores
  maquinas-de-estado.md          — las cuatro máquinas de estado, como tablas normativas
  transport-analysis.md          — justificación de TCP frente a UDP por tipo de mensaje
  sequence-diagrams.md           — el protocolo en movimiento, de extremo a extremo
  logging.md                     — formato del log en consola y archivo, identificación ip:puerto
  despliegue-y-nombres.md        — getaddrinfo(), fallo de resolución, escenarios de despliegue
  trazabilidad-requerimientos.md — mapa requerimiento → documento, decisiones diferidas
server/                          — código del servidor central (C) — se agrega en la Fase 2
```

## Documentos de diseño

Ver `docs/README.md` para el orden de lectura, cómo cada documento se
corresponde con la estructura estilo RFC 793 que recomienda el enunciado, y
cuál manda cuando dos se contradicen. En corto: empezar por
`docs/overview.md` para el panorama general, luego `docs/protocol.md` para
los bytes exactos que viajan por la red, y después `docs/service-spec.md`
junto con `docs/maquinas-de-estado.md` para saber qué hace el servidor con
ellos. `docs/transport-analysis.md` justifica qué mensajes viajan sobre TCP
y cuáles sobre UDP.

Para contrastar el diseño contra el enunciado en lugar de leerlo de
principio a fin, el punto de entrada es
`docs/trazabilidad-requerimientos.md`: mapea cada requerimiento numerado al
documento que lo resuelve, y lista lo que se difirió deliberadamente a la
Fase 2 y a la Fase 3.

## Compilación y ejecución

Todavía no disponible: la implementación del servidor empieza en la Fase 2.
Se compilará con `gcc` mediante un `Makefile` y se ejecutará como:

```
./server <puerto> <archivoDeLogs>
```

Los nodos y los clientes reciben el nombre de host del servidor por línea
de comandos, nunca una IP:

```
./node   <hostname> <puerto> <nombre-del-nodo> <intervalo-segundos>
./client <hostname> <puerto>
```

## Reparto del trabajo

- **Servidor central** — implementado en C usando únicamente la API de
  sockets Berkeley, según exige el enunciado.
- **Nodos y clientes administradores** — pueden implementarse en cualquier
  lenguaje. El contrato entre ellos y el servidor es `docs/protocol.md`; no
  hay ninguna otra dependencia entre las implementaciones.

## Ramas

`main` se mantiene en estado funcional y evaluable en cada fecha de
entrega, porque la evidencia calificada es el historial de commits en sí y
no una única carga final. Una vez iniciada la implementación (Fase 2), el
trabajo se hace en ramas cortas `feature/<descripcion-breve>` que se
integran de vuelta a `main` cuando compilan y hacen lo que dicen. Así
`main` sigue siendo bisecable si algo se rompe más adelante en la Fase 3.

## Commits

El enunciado califica explícitamente que el historial muestre una
**evolución progresiva**: "no se considerará suficiente la existencia de
una única carga final". Un commit gigante por fase se lee como si el
trabajo hubiera ocurrido de una sentada; varios commits pequeños y con
sentido se leen como lo que realmente pasó. En consecuencia: commitear
sobre la marcha, no solo al terminar una funcionalidad completa.

Formato ([Conventional Commits](https://www.conventionalcommits.org/)):

```
<tipo>: <qué cambió, en presente, y por qué si no es evidente>
```

Tipos usados en este repositorio: `feat` (capacidad nueva, p. ej. un tipo
de mensaje que ya se atiende), `fix`, `docs`, `refactor`, `test`, `chore`
(construcción, configuración, herramientas). Ejemplos que encajan en este
proyecto:

```
feat(server): leer el encabezado de 10 bytes antes del payload
feat(node): enviar REGISTER por TCP al arrancar
fix(server): corregir el límite de payload_length por uno
docs: agregar diagrama de secuencia del flujo REGISTER/AUTH/QUERY
```

Mantener el asunto por debajo de unos 70 caracteres y agregar un cuerpo
breve cuando el *porqué* no se deduzca del *qué*.
