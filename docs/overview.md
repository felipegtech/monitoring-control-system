# Visión General del Sistema

## Descripción del problema

Una organización opera una infraestructura distribuida compuesta por nodos
heterogéneos —sensores, equipos de red, dispositivos IoT— que deben ser
supervisados de manera remota. Cada nodo necesita reportar a un punto
central dos cosas cualitativamente distintas:

- **Estado periódico** (telemetría: utilización de CPU, temperatura,
  batería, disponibilidad, métricas de funcionamiento). Es frecuente, y la
  pérdida ocasional de una lectura no importa porque la siguiente la
  reemplaza.
- **Eventos críticos** (fallas, superación de umbrales, pérdida de
  conectividad, alarmas, cambios importantes de estado). Son raros, y
  perder uno puede tener consecuencias reales, así que no puede
  descartarse en silencio como sí puede hacerse con una lectura de
  telemetría.

Un servidor central recibe ambos tipos de información desde muchos nodos a
la vez, mantiene una imagen actualizada de toda la infraestructura, y
responde consultas de uno o más clientes de administración sobre esa
imagen: tanto el estado instantáneo de un nodo como su historial reciente
(al menos 5 lecturas pasadas). Los nodos y los clientes administradores
nunca se comunican directamente entre sí; todo está mediado por el
servidor, que además debe autenticar a los clientes contra cuentas de
usuario reales en lugar de un sistema de login propio, y nunca debe
codificar una dirección IP: todos los extremos se alcanzan resolviendo un
nombre de host.

## Arquitectura propuesta

Tres tipos de entidades, un mediador central, y ningún enlace directo
nodo ↔ cliente:

```
 [Nodo 1]  [Nodo 2]  ...  [Nodo N]         [Cliente admin 1] ... [Cliente admin M]
     \        |             /                      |
      \       |            /                       |
       \      |           /                        |
        TCP (REGISTER,EVENT)   UDP (TELEMETRY)      TCP (AUTH, QUERY)
         \        \              /                 /
          \        \            /                 /
           \        \          /                 /
            v         v       v                 v
           +-------------------------------------+
           |          Servidor Central            |
           |  - un socket TCP en escucha           |
           |  - un socket UDP enlazado              |
           |  - registro de nodos compartido        |
           |    (protegido por mutex)               |
           +-------------------------------------+
```

**Un puerto, dos sockets.** El servidor se invoca como
`server <puerto> <archivoDeLogs>`, con un solo número de puerto. TCP y UDP
tienen espacios de puertos independientes, así que ese único número se usa
para abrir *dos* sockets separados: uno `SOCK_STREAM` sobre el que se hace
`bind()` + `listen()` + `accept()` para REGISTER/EVENT/AUTH/QUERY, y uno
`SOCK_DGRAM` enlazado al mismo número de puerto para TELEMETRY. No entran
en conflicto porque "puerto 5000/tcp" y "puerto 5000/udp" son direcciones
distintas para las tablas de sockets del núcleo. Conviene poder afirmarlo
explícitamente, porque es fácil suponer que un puerto implica un solo
socket.

**Sin IPs codificadas.** Cada participante —nodo o cliente administrador—
se configura con un **nombre de host**, no con una IP, para el servidor al
que se conecta. Cada uno lo resuelve con `getaddrinfo()` inmediatamente
antes de conectarse. Si la resolución falla (nombre mal escrito, sin DNS,
red caída), el participante registra el fallo y reintenta con *backoff*; no
termina su ejecución ni se cae. El servidor no necesita resolver nada hacia
afuera: solo usa `inet_ntop()` del lado receptor para escribir de forma
legible la dirección de quien se conecta. El detalle completo está en
`despliegue-y-nombres.md`.

## Entidades participantes

- **Nodo** — simula un sensor o dispositivo IoT. Se registra una vez por
  TCP y luego, de forma concurrente, envía TELEMETRY periódica por UDP y,
  cuando ocurre algo relevante, un EVENT por TCP sobre la misma conexión
  del REGISTER. Puede implementarse en cualquier lenguaje.
- **Servidor Central** — el componente que este repositorio implementa. C
  puro, únicamente sockets Berkeley. Acepta muchas conexiones TCP
  concurrentes de nodos y clientes (un hilo por conexión) más un único
  socket UDP para toda la telemetría. Mantiene el registro de nodos
  (identidad, estado, lecturas recientes) detrás de un mutex, autentica a
  los clientes administradores contra cuentas reales del sistema mediante
  PAM, y registra cada petición y cada respuesta —con la ip:puerto del
  cliente— tanto en consola como en el archivo de log indicado por línea
  de comandos.
- **Cliente Administrador** — se autentica con AUTH y luego emite mensajes
  QUERY para leer el estado instantáneo de un nodo, su historial (≥5
  puntos) o la lista de nodos conocidos. Puede tener una interfaz sencilla,
  en cualquier lenguaje.
- **PAM / cuentas del sistema** — no es un nodo ni un cliente en el sentido
  del protocolo, pero sí una dependencia externa explícita: es la autoridad
  de autenticación en la que el servidor delega, lo que satisface el
  requerimiento de no construir un sistema de usuarios propio.
