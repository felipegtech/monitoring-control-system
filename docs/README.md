# Índice de la Documentación de Diseño

El requerimiento 8 del enunciado (`assignment-brief.md`) recomienda
estructurar la especificación del protocolo como lo hace el RFC 793 (TCP).
Este índice mapea esa estructura exacta al lugar donde vive cada parte,
para que sea rastreable durante la revisión y la sustentación en lugar de
quedar implícita.

| Sección estilo RFC 793 | Dónde está |
|---|---|
| (a) Visión general del protocolo | `overview.md` |
| (b) Especificación del servicio (primitivas) | `service-spec.md` → "Primitivas del servicio" |
| (c) Formato de mensajes (encabezado) | `protocol.md` |
| (d) Reglas de procedimiento (flujo, máquinas de estado) | `service-spec.md` (prosa) y `maquinas-de-estado.md` (tablas normativas) |
| (e) Ejemplos de implementación (diagramas de secuencia) | `sequence-diagrams.md` |

Documentos de apoyo, fuera de la estructura RFC propiamente dicha:

| Documento | Qué cubre |
|---|---|
| `transport-analysis.md` | El "análisis preliminar sobre el uso de TCP y UDP" que la Fase 1 pide por separado |
| `despliegue-y-nombres.md` | Requerimiento 4: resolución de nombres, manejo del fallo de resolución, escenarios de despliegue |
| `logging.md` | Formato exacto del log en consola y archivo, con el identificador ip:puerto exigido |
| `trazabilidad-requerimientos.md` | Mapa requerimiento → documento, y las decisiones diferidas |
| `assignment-brief.md` | Copia literal de los requerimientos y la rúbrica |

## Orden de lectura

1. `overview.md` — qué problema se resuelve y cuál es la arquitectura.
2. `protocol.md` — los bytes exactos que viajan por la red.
3. `service-spec.md` — qué hace el servidor con esos bytes, y cuándo.
4. `maquinas-de-estado.md` — las mismas reglas como tablas normativas de
   transición.
5. `transport-analysis.md` — por qué cada mensaje viaja sobre TCP o UDP.
6. `sequence-diagrams.md` — todo lo anterior, ilustrado de extremo a
   extremo.

`despliegue-y-nombres.md` y `logging.md` se leen al implementar, no al
aprender el protocolo. `trazabilidad-requerimientos.md` es el punto de
entrada para revisar: dice qué documento responde a cada requerimiento.

## Precedencia normativa

Cuando dos documentos se contradigan, manda el más específico:

1. `protocol.md` — disposición de bytes, tamaños, límites.
2. `maquinas-de-estado.md` — transiciones, condiciones, acciones.
3. `service-spec.md` — todo lo demás.

`service-spec.md` describe los mismos ciclos de vida en prosa por
legibilidad; `maquinas-de-estado.md` es lo que sigue quien implementa
cuando ambos están redactados de forma distinta.

## Idioma

Toda la documentación está en español. Los identificadores del protocolo
—`REGISTER`, `TELEMETRY`, `entity_id`, `MAX_PAYLOAD`— se mantienen en
inglés porque son nombres que aparecerán literalmente en el código C y en
los mensajes que viajan por la red; traducirlos en la documentación haría
que el documento y el código dejaran de coincidir.
