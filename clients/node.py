#!/usr/bin/env python3
"""
node.py — nodo sensor/IoT simulado.

    python3 clients/node.py <hostname> <puerto> <nombre-del-nodo> <intervalo-s>
                            [--type N] [--threshold X] [--base X] [--seed N]

Sigue la máquina de estados del nodo (docs/maquinas-de-estado.md § 1):

    DESCONECTADO -> RESOLVIENDO -> CONECTANDO -> REGISTRANDO -> ACTIVO

- REGISTER y EVENT viajan por TCP sobre una misma conexión que se mantiene
  abierta: su cierre es la señal de desconexión para el servidor.
- TELEMETRY viaja por UDP cada <intervalo> segundos, sin confirmación.
- Los EVENT sí requieren confirmación: quedan en una cola de pendientes y
  solo se retiran cuando llega RESP(EVENT). Si la conexión cae antes, se
  reenvían tras el siguiente REGISTER (entrega al menos una vez).
- Un fallo de resolución de nombres o de conexión nunca termina el proceso:
  se reintenta con backoff 1, 2, 4, 8, 16 y 30 s.

Comandos por stdin mientras corre: `evento <texto>`, `falla`, `estado`,
`salir`.
"""

import argparse
import collections
import random
import select
import socket
import struct
import sys
import threading
import time

import mcsproto as P

REGISTER_TIMEOUT = 5.0   # s esperando RESP(REGISTER)
EVENT_TIMEOUT = 5.0      # s esperando RESP(EVENT)


def log(state, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"{ts} [{state:<12}] {msg}", flush=True)


class Node:
    def __init__(self, args):
        self.host = args.host
        self.port = args.port
        self.name = args.name
        self.interval = args.interval
        self.node_type = args.type
        self.threshold = args.threshold
        self.value = args.base
        self.base = args.base
        self.rng = random.Random(args.seed)

        self.state = "DESCONECTADO"
        self.node_id = 0                  # asignado por el servidor
        self.tcp = None
        self.udp = None
        self.udp_addr = None
        self.lock = threading.Lock()      # protege node_id / udp_addr
        self.pending = collections.deque()  # EVENT sin confirmar
        self.pending_lock = threading.Lock()
        self.wakeup = threading.Event()
        self.running = True
        self.above = False                # para detectar el cruce de umbral

    # ------------------------------------------------------ máquina principal

    def set_state(self, state, msg=""):
        self.state = state
        if msg:
            log(state, msg)

    def run(self):
        threading.Thread(target=self.telemetry_loop, daemon=True).start()
        threading.Thread(target=self.stdin_loop, daemon=True).start()
        attempt = 0
        while self.running:
            ok = self.connect_and_register()
            if ok:
                attempt = 0
                self.active_loop()          # vuelve cuando se cae la conexión
                if not self.running:
                    break
            delay = P.BACKOFF[min(attempt, len(P.BACKOFF) - 1)]
            attempt += 1
            self.set_state("DESCONECTADO", f"reintento en {delay} s")
            self.sleep(delay)
        self.close_tcp()
        log(self.state, "apagado ordenado")

    def sleep(self, seconds):
        end = time.monotonic() + seconds
        while self.running and time.monotonic() < end:
            time.sleep(min(0.2, end - time.monotonic()))

    def connect_and_register(self):
        # RESOLVIENDO + CONECTANDO: connect_tcp recorre toda la lista de
        # direcciones que devuelve getaddrinfo().
        self.set_state("RESOLVIENDO", f"getaddrinfo('{self.host}', {self.port})")
        try:
            sock = P.connect_tcp(self.host, self.port)
        except socket.gaierror as exc:
            log("RESOLVIENDO", P.describe_gaierror(exc))
            return False
        except OSError as exc:
            log("CONECTANDO", f"connect() falló: {exc.strerror or exc}")
            return False
        self.tcp = sock
        peer = sock.getpeername()
        host = f"[{peer[0]}]" if ":" in peer[0] else peer[0]
        self.set_state("CONECTANDO", f"conexión TCP con {host}:{peer[1]}")

        if not self.register():
            self.close_tcp()
            return False

        # Socket UDP de telemetría: se resuelve una vez, con la misma
        # familia que la conexión TCP que funcionó.
        try:
            infos = P.resolve(self.host, self.port, socket.SOCK_DGRAM,
                              sock.family)
            family, _, _, _, addr = infos[0]
        except (socket.gaierror, IndexError):
            family, addr = sock.family, sock.getpeername()
        with self.lock:
            if self.udp is None or self.udp.family != family:
                if self.udp:
                    self.udp.close()
                self.udp = socket.socket(family, socket.SOCK_DGRAM)
            self.udp_addr = addr
        return True

    def register(self):
        self.set_state("REGISTRANDO",
                       f"REGISTER name='{self.name}' type={self.node_type} "
                       f"interval={self.interval}")
        try:
            self.tcp.sendall(P.encode_register(self.name, self.node_type,
                                               self.interval))
            self.tcp.settimeout(REGISTER_TIMEOUT)
            _, body = P.read_response(self.tcp, P.REGISTER)
            self.tcp.settimeout(None)
            (node_id,) = struct.unpack("!I", body)
        except P.ServerError as exc:
            log("REGISTRANDO", f"ERROR del servidor: {exc}")
            if exc.code == P.NAME_IN_USE:
                log("REGISTRANDO", "otro nodo conectado ya usa ese nombre")
            return False
        except socket.timeout:
            log("REGISTRANDO", "expiró el temporizador de registro")
            return False
        except (OSError, P.ConnectionClosed, P.ProtocolError) as exc:
            log("REGISTRANDO", f"fallo durante el registro: {exc}")
            return False
        with self.lock:
            self.node_id = node_id
        self.set_state("ACTIVO", f"registrado con node_id={node_id}")
        return True

    def active_loop(self):
        """ACTIVO: envía los EVENT pendientes y vigila el cierre de TCP."""
        while self.running:
            if not self.flush_events():
                break
            # El servidor nunca envía nada sin que se le pida: si el socket
            # se vuelve legible es porque cerró (o envió basura).
            try:
                r, _, _ = select.select([self.tcp], [], [], 0)
                if r and not self.tcp.recv(1, socket.MSG_PEEK):
                    log("ACTIVO", "el servidor cerró la conexión TCP")
                    break
            except OSError as exc:
                log("ACTIVO", f"error en la conexión TCP: {exc}")
                break
            self.wakeup.wait(0.5)
            self.wakeup.clear()
        with self.lock:
            self.node_id = 0              # detiene la telemetría
        self.close_tcp()

    def flush_events(self):
        """Envía cada EVENT pendiente y espera su RESP. False = conexión rota."""
        while self.running:
            with self.pending_lock:
                if not self.pending:
                    return True
                code, sev, text, node_ts = self.pending[0]
            with self.lock:
                node_id = self.node_id
            try:
                self.tcp.sendall(P.encode_event(node_id, code, sev, text,
                                                node_ts))
                self.tcp.settimeout(EVENT_TIMEOUT)
                P.read_response(self.tcp, P.EVENT)
                self.tcp.settimeout(None)
            except P.ServerError as exc:
                if exc.code == P.UNKNOWN_NODE:
                    # ACTIVO -> REGISTRANDO sobre la misma conexión
                    log("ACTIVO", "ERROR(UNKNOWN_NODE): se vuelve a registrar")
                    with self.lock:
                        self.node_id = 0
                    if not self.register():
                        return False
                    continue
                log("ACTIVO", f"EVENT rechazado: {exc}; se descarta")
                with self.pending_lock:
                    self.pending.popleft()
                continue
            except socket.timeout:
                log("ACTIVO", "sin RESP(EVENT) a tiempo; se reconecta y reenvía")
                return False
            except (OSError, P.ConnectionClosed, P.ProtocolError) as exc:
                log("ACTIVO", f"fallo enviando EVENT: {exc}; queda pendiente")
                return False
            with self.pending_lock:
                self.pending.popleft()
            log("ACTIVO", f"EVENT confirmado: {P.EVENT_NAMES.get(code, code)} "
                          f"[{P.SEVERITY_NAMES.get(sev, sev)}] {text}")
        return True

    def close_tcp(self):
        if self.tcp:
            try:
                self.tcp.close()
            except OSError:
                pass
            self.tcp = None

    def queue_event(self, code, severity, text):
        with self.pending_lock:
            self.pending.append((code, severity, text, int(time.time())))
        self.wakeup.set()

    # ---------------------------------------------------------- telemetría

    def next_value(self):
        """Paseo aleatorio alrededor de `base`, con picos ocasionales."""
        drift = (self.base - self.value) * 0.1
        self.value += drift + self.rng.uniform(-1.5, 1.5)
        if self.rng.random() < 0.05:
            self.value += self.rng.uniform(10, 25)     # pico para la demo
        return self.value

    def telemetry_loop(self):
        while self.running:
            start = time.monotonic()
            with self.lock:
                node_id, udp, addr = self.node_id, self.udp, self.udp_addr
            if node_id and udp and addr:
                value = self.next_value()
                try:
                    udp.sendto(P.encode_telemetry(node_id, self.node_type,
                                                  value), addr)
                    unit = P.METRIC_UNITS.get(self.node_type, "")
                    log("ACTIVO", f"TELEMETRY {value:.2f}{unit} (UDP)")
                except OSError as exc:
                    log("ACTIVO", f"sendto() falló: {exc}")
                self.check_threshold(value)
            elapsed = time.monotonic() - start
            self.sleep(max(0.0, self.interval - elapsed))

    def check_threshold(self, value):
        if value > self.threshold and not self.above:
            self.above = True
            self.queue_event(P.EV_THRESHOLD_EXCEEDED, P.SEV_CRITICAL,
                             f"valor {value:.1f} excede el umbral "
                             f"{self.threshold:.1f}")
        elif value <= self.threshold and self.above:
            self.above = False
            self.queue_event(P.EV_BACK_TO_NORMAL, P.SEV_INFO,
                             f"valor {value:.1f} volvió bajo el umbral")

    # --------------------------------------------------------------- stdin

    def stdin_loop(self):
        for line in sys.stdin:
            cmd, _, rest = line.strip().partition(" ")
            if cmd == "evento":
                self.queue_event(P.EV_MANUAL, P.SEV_WARNING, rest or "evento manual")
            elif cmd == "falla":
                self.queue_event(P.EV_HARDWARE_FAULT, P.SEV_CRITICAL,
                                 rest or "falla de hardware simulada")
            elif cmd == "estado":
                with self.pending_lock:
                    n = len(self.pending)
                log(self.state, f"node_id={self.node_id} eventos_pendientes={n}")
            elif cmd == "salir":
                self.running = False
                self.wakeup.set()
                return
            elif cmd:
                print("comandos: evento <texto> | falla [texto] | estado | salir")


def main():
    ap = argparse.ArgumentParser(description="Nodo sensor simulado")
    ap.add_argument("host", help="nombre de host del servidor (no una IP fija)")
    ap.add_argument("port", type=int)
    ap.add_argument("name", help="nombre del nodo")
    ap.add_argument("interval", type=int, help="segundos entre TELEMETRY")
    ap.add_argument("--type", type=int, default=1,
                    help="tipo de nodo/métrica: 1=TEMPERATURE 2=HUMIDITY "
                         "3=MOTION 4=CPU 5=BATTERY (def. 1)")
    ap.add_argument("--base", type=float, default=25.0,
                    help="valor típico de la métrica (def. 25)")
    ap.add_argument("--threshold", type=float, default=40.0,
                    help="umbral que dispara un EVENT crítico (def. 40)")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()
    if not 1 <= args.port <= 65535 or args.interval < 1:
        ap.error("puerto 1..65535 e intervalo >= 1")
    if not 0 < len(args.name.encode()) <= 255:
        ap.error("el nombre debe tener entre 1 y 255 bytes")

    node = Node(args)
    try:
        node.run()
    except KeyboardInterrupt:
        node.running = False
        node.close_tcp()
        log(node.state, "interrumpido (Ctrl+C)")


if __name__ == "__main__":
    main()
