#!/usr/bin/env python3
"""
admin.py — cliente administrador.

    python3 clients/admin.py <hostname> <puerto> [-u usuario]

Se autentica con AUTH (credenciales reales del sistema, verificadas por el
servidor con PAM) y luego permite consultar la infraestructura:

    nodos                     lista de nodos y su estado (LIST_NODES)
    estado <id>               estado instantáneo de un nodo (STATUS)
    historial <id> [n]        últimas n lecturas, por omisión 5 (HISTORY)
    eventos <id> [n]          últimos eventos críticos del nodo (EVENTS)
    purgar <id>               elimina un nodo DISCONNECTED (solo ADMIN)
    panel [segundos]          tablero que se refresca solo (Ctrl+C para salir)
    ayuda | salir

Si la conexión se cae, el cliente se reconecta y vuelve a autenticarse con
las mismas credenciales, que solo conserva en memoria durante la sesión.
"""

import argparse
import datetime
import getpass
import os
import shlex
import socket
import sys
import time

import mcsproto as P

USE_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None
STATE_COLORS = {0: "36", 1: "32", 2: "33", 3: "31"}   # cian, verde, amarillo, rojo
SEV_COLORS = {0: "36", 1: "33", 2: "31"}


def color(text, code):
    return f"\033[{code}m{text}\033[0m" if USE_COLOR and code else text


def fmt_state(state):
    return color(P.STATE_NAMES.get(state, str(state)), STATE_COLORS.get(state))


def fmt_ts(ts):
    if not ts:
        return "—"
    return datetime.datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S")


def fmt_age(ts):
    if not ts:
        return ""
    return f"(hace {max(0, int(time.time() - ts))} s)"


class Session:
    """Conexión autenticada con el servidor."""

    def __init__(self, host, port, username, password):
        self.host, self.port = host, port
        self.username, self.password = username, password
        self.sock = None
        self.session_id = 0
        self.profile = 0

    def connect(self, max_attempts=None):
        """Resuelve, conecta y autentica, con backoff ante fallos de red."""
        attempt = 0
        while True:
            try:
                self.sock = P.connect_tcp(self.host, self.port)
                break
            except socket.gaierror as exc:
                problem = P.describe_gaierror(exc)
            except OSError as exc:
                problem = f"connect() falló: {exc.strerror or exc}"
            attempt += 1
            if max_attempts and attempt >= max_attempts:
                raise ConnectionError(problem)
            delay = P.BACKOFF[min(attempt - 1, len(P.BACKOFF) - 1)]
            print(color(f"! {problem}; reintento en {delay} s (Ctrl+C para "
                        f"cancelar)", "33"))
            time.sleep(delay)
        self.sock.sendall(P.encode_auth(self.username, self.password))
        sid, body = P.read_response(self.sock, P.AUTH)
        self.session_id, self.profile = P.parse_auth(body)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def query(self, qtype, target=0, count=0):
        """Envía un QUERY y devuelve el cuerpo de la RESP. Reintenta una vez
        si la conexión se cayó, reconectando y reautenticando."""
        for retry in (False, True):
            try:
                if self.sock is None:
                    self.connect()
                self.sock.sendall(P.encode_query(self.session_id, qtype,
                                                 target, count))
                _, body = P.read_response(self.sock, P.QUERY)
                return body
            except (P.ConnectionClosed, ConnectionResetError,
                    BrokenPipeError) as exc:
                self.close()
                if retry:
                    raise ConnectionError(f"conexión perdida: {exc}")
                print(color("! conexión perdida; reconectando...", "33"))


# ----------------------------------------------------------- presentación

def show_list(s):
    nodes = P.parse_list(s.query(P.Q_LIST_NODES))
    if not nodes:
        print("no hay nodos registrados")
        return nodes
    print(f"{'ID':>5}  {'NOMBRE':<24} ESTADO")
    for n in nodes:
        print(f"{n['node_id']:>5}  {n['name']:<24} {fmt_state(n['state'])}")
    return nodes


def show_status(s, node_id):
    st = P.parse_status(s.query(P.Q_STATUS, node_id))
    print(f"nodo {st['node_id']}: {fmt_state(st['state'])}")
    if st["last_timestamp"]:
        print(f"  última lectura: {st['last_value']:.2f} el "
              f"{fmt_ts(st['last_timestamp'])} {fmt_age(st['last_timestamp'])}")
    else:
        print("  aún no ha llegado telemetría")


def show_history(s, node_id, count=5):
    pts = P.parse_history(s.query(P.Q_HISTORY, node_id, count))
    if not pts:
        print(f"nodo {node_id}: sin lecturas todavía")
        return
    print(f"nodo {node_id}: últimas {len(pts)} lecturas (más reciente primero)")
    values = [v for _, v in pts]
    lo, hi = min(values), max(values)
    for ts, v in pts:
        bar = "█" * (1 + int(29 * (v - lo) / (hi - lo))) if hi > lo else "█"
        print(f"  {fmt_ts(ts)}  {v:>8.2f}  {bar}")


def show_events(s, node_id, count=5):
    evs = P.parse_events(s.query(P.Q_EVENTS, node_id, count))
    if not evs:
        print(f"nodo {node_id}: sin eventos")
        return
    for e in evs:
        sev = color(P.SEVERITY_NAMES.get(e["severity"], e["severity"]),
                    SEV_COLORS.get(e["severity"]))
        print(f"  {fmt_ts(e['server_ts'])}  {sev:<8} "
              f"{P.EVENT_NAMES.get(e['code'], e['code'])}: {e['message']}")


def purge(s, node_id):
    s.query(P.Q_PURGE_NODE, node_id)
    print(f"nodo {node_id} purgado del registro")


def panel(s, every):
    """Tablero: estado instantáneo + 5 lecturas históricas de cada nodo."""
    try:
        while True:
            nodes = P.parse_list(s.query(P.Q_LIST_NODES))
            out = [f"Panel de monitoreo — {s.host}:{s.port} — "
                   f"{time.strftime('%H:%M:%S')} (Ctrl+C para volver)", ""]
            if not nodes:
                out.append("no hay nodos registrados")
            for n in nodes:
                try:
                    st = P.parse_status(s.query(P.Q_STATUS, n["node_id"]))
                    hist = P.parse_history(s.query(P.Q_HISTORY, n["node_id"], 5))
                    evs = P.parse_events(s.query(P.Q_EVENTS, n["node_id"], 1))
                except P.ServerError:
                    continue      # purgado entre LIST_NODES y STATUS
                last = (f"{st['last_value']:8.2f} {fmt_age(st['last_timestamp'])}"
                        if st["last_timestamp"] else "     sin telemetría")
                out.append(f"[{n['node_id']:>3}] {n['name']:<22} "
                           f"{fmt_state(n['state']):<22} {last}")
                out.append("      historial: " +
                           "  ".join(f"{v:.1f}" for _, v in hist))
                if evs:
                    e = evs[0]
                    out.append("      último evento: " + color(
                        f"{P.SEVERITY_NAMES.get(e['severity'])} "
                        f"{e['message']} ({fmt_ts(e['server_ts'])})",
                        SEV_COLORS.get(e["severity"])))
                out.append("")
            if USE_COLOR:
                print("\033[2J\033[H", end="")
            print("\n".join(out), flush=True)
            time.sleep(every)
    except KeyboardInterrupt:
        print()


HELP = __doc__.split("permite consultar la infraestructura:")[1].split(
    "Si la conexión")[0]


def need_id(args, n=1):
    if len(args) < n or not all(a.isdigit() for a in args[:n]):
        raise ValueError("se esperaba un id de nodo numérico")
    return [int(a) for a in args]


def repl(s):
    print(f"autenticado como {s.username} — perfil "
          f"{P.PROFILE_NAMES.get(s.profile, s.profile)}, session_id={s.session_id}")
    print("escriba 'ayuda' para ver los comandos")
    while True:
        try:
            line = input(color("monitoreo> ", "1"))
        except (EOFError, KeyboardInterrupt):
            print()
            return
        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"entrada inválida: {exc}")
            continue
        if not parts:
            continue
        cmd, args = parts[0].lower(), parts[1:]
        try:
            if cmd in ("salir", "exit", "quit"):
                return
            elif cmd in ("ayuda", "help", "?"):
                print(HELP)
            elif cmd in ("nodos", "list", "ls"):
                show_list(s)
            elif cmd in ("estado", "status"):
                show_status(s, *need_id(args))
            elif cmd in ("historial", "history"):
                show_history(s, *need_id(args, 1)[:2])
            elif cmd in ("eventos", "events"):
                show_events(s, *need_id(args, 1)[:2])
            elif cmd in ("purgar", "purge"):
                purge(s, *need_id(args))
            elif cmd in ("panel", "watch"):
                panel(s, int(args[0]) if args and args[0].isdigit() else 2)
            else:
                print(f"comando desconocido: {cmd} (escriba 'ayuda')")
        except P.ServerError as exc:
            print(color(f"ERROR del servidor — {exc.code_name}: {exc.message}",
                        "31"))
        except ValueError as exc:
            print(f"uso incorrecto: {exc}")
        except (ConnectionError, P.ProtocolError, OSError) as exc:
            print(color(f"fallo de comunicación: {exc}", "31"))


def main():
    ap = argparse.ArgumentParser(description="Cliente administrador")
    ap.add_argument("host", help="nombre de host del servidor (no una IP fija)")
    ap.add_argument("port", type=int)
    ap.add_argument("-u", "--user", default=None)
    ap.add_argument("-c", "--command", default=None,
                    help="ejecuta un solo comando y sale (p. ej. -c nodos)")
    args = ap.parse_args()

    username = args.user or input("usuario: ")
    # Hasta 3 intentos: el servidor cierra la conexión al tercer fallo.
    s = None
    for attempt in range(1, 4):
        password = os.environ.get("MCS_PASSWORD") or getpass.getpass(
            f"contraseña de {username}: ")
        try:
            if s is None or s.sock is None:
                s = Session(args.host, args.port, username, password)
                s.connect()
            else:
                s.password = password
                s.sock.sendall(P.encode_auth(username, password))
                _, body = P.read_response(s.sock, P.AUTH)
                s.session_id, s.profile = P.parse_auth(body)
            break
        except P.ServerError as exc:
            print(color(f"ERROR del servidor — {exc.code_name}: {exc.message} "
                        f"(intento {attempt}/3)", "31"))
            if exc.code != P.AUTH_FAILED or os.environ.get("MCS_PASSWORD"):
                sys.exit(1)
        except (P.ConnectionClosed, P.ProtocolError, OSError) as exc:
            print(color(f"no se pudo autenticar: {exc}", "31"))
            sys.exit(1)
        except KeyboardInterrupt:
            print()
            sys.exit(130)
    else:
        print("demasiados intentos fallidos")
        sys.exit(1)

    try:
        if args.command:
            parts = shlex.split(args.command)
            cmd = {"nodos": lambda: show_list(s),
                   "estado": lambda: show_status(s, *need_id(parts[1:])),
                   "historial": lambda: show_history(s, *need_id(parts[1:])[:2]),
                   "eventos": lambda: show_events(s, *need_id(parts[1:])[:2]),
                   "purgar": lambda: purge(s, *need_id(parts[1:]))}.get(parts[0])
            if not cmd:
                sys.exit(f"comando desconocido: {parts[0]}")
            try:
                cmd()
            except P.ServerError as exc:
                sys.exit(f"ERROR del servidor — {exc.code_name}: {exc.message}")
        else:
            repl(s)
    finally:
        s.close()


if __name__ == "__main__":
    main()
