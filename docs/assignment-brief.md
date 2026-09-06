# Enunciado del Proyecto (copia literal)

Curso: Internet: Arquitectura y Protocolos, Telemática — EAFIT, 2026-2
Proyecto: Sistema de monitoreo y control distribuido

Se conserva aquí de forma literal como fuente autoritativa de los
requerimientos y de la rúbrica de calificación, para que cada decisión de
diseño pueda rastrearse hasta un número de requerimiento concreto durante
la sustentación. El mapa completo de esa trazabilidad está en
`trazabilidad-requerimientos.md`.

## Requerimientos

1) La arquitectura mínima del sistema consistirá en al menos 2 nodos que
   reportan métricas, un servidor central y uno o varios clientes de
   administración que pueden visualizar la información. Los detalles
   específicos de la arquitectura podrán ser definidos por cada grupo,
   siempre que se cumplan los requerimientos mínimos del proyecto.
2) Los nodos envían información directamente al servidor central y los
   clientes consultan la información desde el servidor. No hay
   comunicación directa entre nodos y clientes.
3) El servidor deberá:
   a) registrar los nodos que participan en el sistema
   b) recibir información periódica sobre su estado
   c) recibir eventos generados por los nodos
   d) mantener información sobre el estado de los nodos conectados
   e) responder consultas realizadas sobre la infraestructura
   f) atender múltiples clientes de manera concurrente.
4) El código no puede tener direcciones IP codificadas. Debe resolver
   nombres de dominio. Si la resolución falla, el servicio debe manejar la
   excepción sin finalizar su ejecución.
5) Para los clientes se recomienda implementar una interfaz de
   visualización sencilla, donde puedan leer los estados instantáneos de
   los nodos y, al menos, 5 datos históricos de los mismos.
6) El sistema debe incorporar un mecanismo de autenticación y gestión de
   perfiles de usuario para controlar el acceso de diferentes clientes a
   los servicios ofrecidos. Se recomienda que los grupos eviten
   implementar un sistema de usuarios basado únicamente en archivos
   locales o bases de datos propias dentro de la aplicación principal.
7) Cada grupo deberá diseñar un protocolo de capa de aplicación que
   permita la comunicación entre las entidades definidas en la
   arquitectura. El protocolo deberá incluir mensajes que permitan, como
   mínimo:
   a) registro de un nodo
   b) envío de información periódica de estado
   c) envío de un evento
   d) confirmación o respuesta cuando sea necesaria
   e) consulta de información
   f) respuesta a una consulta
   g) reporte de errores
8) Especificación del servicio (estilo RFC, p. ej. RFC 793):
   a) Visión general del protocolo
   b) Especificación del servicio (primitivas)
   c) Formato de mensajes (encabezado)
   d) Reglas de procedimiento (flujo de operación, máquinas de estado)
   e) Ejemplos de implementación (código o diagramas de secuencia)
9) El protocolo se integra a TCP/IP vía sockets, implementado en cliente y
   servidor. Cada grupo decide el protocolo de transporte adecuado.
10) El protocolo debe soportar múltiples clientes simultáneos (se permite
    el uso de hilos).
11) El protocolo debe implementar control de excepciones: conexiones
    fallidas, mensajes con formato incorrecto, y demás situaciones
    pertinentes.

## Detalles de implementación

- Clientes/nodos: cualquier lenguaje (Python, C, Java...), UI simple
  opcional.
- Servidor: **solo C**, **solo API de Sockets Berkeley**.
- El servidor debe loguear en consola y en archivo cada petición/respuesta,
  incluyendo ip:puerto del cliente origen.
- El servidor se ejecuta como: `servidor <puerto> <archivoDeLogs>`.
- Compilación recomendada: gcc + Makefile.
- El servidor puede desplegarse en la nube (AWS Educate / IaaS).

## Selección del protocolo de transporte

Debe analizarse, por tipo de mensaje: frecuencia de transmisión, tamaño,
criticidad, tolerancia a pérdidas, necesidad de orden, necesidad de
confiabilidad, necesidad de mantener una conexión, comportamiento ante
retardos, comportamiento ante pérdida de mensajes. La arquitectura final
puede usar solo TCP, solo UDP, o una combinación — la decisión debe estar
técnicamente justificada. Si se usa UDP para algo que requiere alguna
confiabilidad, se deben definir mecanismos propios de capa de aplicación
(ACK, identificación de mensajes, temporizadores, retransmisión, detección
de duplicados, control de desorden) — solo donde corresponda, no se espera
reimplementar TCP.

## Evaluación

- **Fase 1 — Diseño y arquitectura** (0.5 unidades, entrega 9 sept):
  propuesta inicial con descripción general del problema, arquitectura
  propuesta, entidades participantes, tipos de mensajes, sintaxis
  preliminar, reglas básicas de comunicación, máquinas de estado, análisis
  preliminar TCP/UDP. Posible exposición de 5 minutos para grupos
  seleccionados.
- **Fase 2 — Comunicación básica** (0.5 unidades, entrega 23 sept):
  creación/configuración de sockets, establecimiento de comunicación,
  envío/recepción/interpretación de mensajes, construcción de respuestas,
  reglas básicas del protocolo implementadas y demostrables.
- **Fase 3 — Concurrencia, resiliencia, pruebas** (4 unidades, entrega 30
  sept, sustentación oral presencial): múltiples clientes/nodos
  concurrentes, control de errores (formato incorrecto, parámetros
  inválidos, desconexiones, fallas de comunicación, nodos desconocidos,
  expiración de temporizadores, pérdida/duplicación de mensajes cuando
  corresponda).

### Rúbrica de sustentación (Fase 3)

1. Programación del Servidor — 40%
   - Implementación de la API de Sockets — 5%
   - Manejo de Múltiples Clientes — 10%
   - Lógica del Protocolo — 25%
2. Programación de los Clientes — 30%
   - Conexión y Petición — 15%
   - Manejo de Respuestas — 15%
3. Funcionamiento y Comunicación — 20%
   - Robustez del Protocolo — 10%
   - Coordinación de Mensajes — 10%
4. Documentación e Informe — 10%
   - Especificación del Protocolo — 10%

## Entrega

- Proyecto en equipo (no individual).
- Repositorio propio, no compartido con otros grupos. Se evalúa la
  **evolución progresiva** a través del historial de commits — una única
  carga final no es suficiente.
- Entrega final: antes del 30 de sept de 2026, 23:59. Enlace al repo en el
  buzón de interactiva virtual. No debe haber commits posteriores a esa
  fecha al momento de la sustentación.
