# Análisis de la Capa de Transporte

El enunciado pide este análisis como pieza propia, evaluada contra un
conjunto específico de criterios, en lugar de dejarlo implícito en las
definiciones de los mensajes. Cada tipo de mensaje se evalúa abajo contra
todos ellos, y la elección de transporte se desprende de esa evaluación, no
al revés.

## Criterios aplicados a cada tipo de mensaje

| Criterio | REGISTER | EVENT | TELEMETRY | AUTH / QUERY |
|---|---|---|---|---|
| Frecuencia de transmisión | Una sola vez por sesión del nodo | Rara (solo cuando ocurre un evento) | Alta (periódica, p. ej. cada pocos segundos) | Rara o moderada (una vez por sesión de cliente, luego una por consulta) |
| Tamaño del mensaje | Pequeño (nombre + tipo) | Pequeño a moderado (incluye texto libre) | Muy pequeño (13 bytes de payload) | Pequeño (credenciales o parámetros de consulta) |
| Criticidad de la información | Alta — sin esto el nodo no existe para el sistema | Alta — puede representar una falla real | Baja individualmente — el siguiente dato la reemplaza | Alta — controla el acceso al sistema |
| Tolerancia a pérdidas | Ninguna | Ninguna | Alta | Ninguna |
| Necesidad de orden | No aplica (un solo mensaje) | Deseable pero no crítico (el timestamp del servidor resuelve el orden real) | No — cada lectura es independiente | Sí, dentro de una sesión (AUTH antes que cualquier QUERY) |
| Necesidad de confiabilidad | Total | Total | Ninguna a nivel de mensaje individual | Total |
| Necesidad de mantener una conexión | Sí — la conexión permanece abierta y es la señal de desconexión del nodo | Sí — reutiliza la conexión del REGISTER | No — no orientado a conexión | Sí — la sesión autenticada vive mientras viva la conexión TCP |
| Comportamiento ante retardos | Aceptable esperar la confirmación | Aceptable esperar la confirmación | Un retardo alto simplemente deja el dato desactualizado hasta la siguiente lectura | Aceptable esperar la respuesta |
| Comportamiento ante pérdida de mensajes | Inaceptable — debe reintentarse a nivel de aplicación si la conexión falla antes de la respuesta | Inaceptable — un evento crítico perdido puede ocultar una falla real | Aceptable — se descarta y se espera la siguiente lectura | Inaceptable — una consulta perdida deja al cliente sin respuesta |

## Decisión resultante

- **TCP (`SOCK_STREAM`)** para REGISTER, EVENT, AUTH, QUERY, RESP y ERROR.
  Todos requieren entrega confirmada, necesitan el emparejamiento
  petición/respuesta que TCP da de forma natural, y en el caso de REGISTER
  y EVENT dependen explícitamente del estado de conexión de TCP como única
  señal fiable de que un nodo sigue presente. Esa señal sencillamente no
  existe en un protocolo sin conexión.
- **UDP (`SOCK_DGRAM`)** únicamente para TELEMETRY. Es el único tipo de
  mensaje en el que todos los criterios apuntan en la misma dirección:
  frecuente, diminuto, de bajo valor individual, sin requisito de orden
  entre lecturas, y activamente mejor *sin* pagar el costo de conexión y
  retransmisión, porque la retransmisión tardía de una lectura vieja es
  menos útil que simplemente esperar la siguiente.

## Por qué no se agregó confiabilidad de aplicación sobre UDP

El enunciado permite —y espera, "cuando corresponda"— agregar
confirmación, retransmisión, detección de duplicados o reordenamiento
sobre UDP si algún mensaje enviado por ese transporte necesita realmente
alguna de esas garantías.

TELEMETRY es deliberadamente el *único* tipo de mensaje sobre UDP, y se
eligió UDP precisamente porque **no** necesita ninguna de ellas. Montarle
encima un esquema de confirmación y retransmisión sería reimplementar el
trabajo de TCP para unos datos que se pusieron en UDP justamente porque no
lo requieren. La consecuencia de esa elección no queda oculta: el estado
`STALE` existe para hacer visible al administrador que la telemetría dejó
de llegar, sin necesidad de que el protocolo confirme cada datagrama.

Si un requerimiento futuro exigiera que TELEMETRY fuera consciente de las
pérdidas —por ejemplo, para contar cuántas lecturas consecutivas se
perdieron y derivar una métrica de calidad de señal— el mecanismo a
agregar sería un número de secuencia monótono por nodo en cada datagrama,
con el servidor llevando el último número visto por nodo para detectar
huecos. Queda anotado aquí como no-requerimiento documentado, no como
omisión.
