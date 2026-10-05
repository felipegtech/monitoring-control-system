"""
mcsproto.py — implementación en Python del protocolo de docs/protocol.md,
compartida por el nodo (node.py) y el cliente administrador (admin.py).

Todo entero de más de un byte viaja en orden de red (big-endian): en
`struct` eso es el prefijo "!". El float de TELEMETRY es IEEE-754 de 4 bytes
("!f").
"""

import errno
import socket
import struct
import time

PROTO_VERSION = 1
HEADER_SIZE = 10           # 1 + 1 + 4 + 4: son 10 bytes, no 8
MAX_PAYLOAD = 4096
HEADER_FMT = "!BBII"       # version, msg_type, entity_id, payload_length

# msg_type
REGISTER, EVENT, TELEMETRY, QUERY, RESP, ERROR, AUTH = 1, 2, 3, 4, 5, 6, 7
MSG_NAMES = {1: "REGISTER", 2: "EVENT", 3: "TELEMETRY", 4: "QUERY",
             5: "RESP", 6: "ERROR", 7: "AUTH"}

# query_type
Q_STATUS, Q_HISTORY, Q_LIST_NODES, Q_EVENTS, Q_PURGE_NODE = 1, 2, 3, 4, 5

# error_code
ERROR_NAMES = {
    1: "AUTH_FAILED", 2: "UNKNOWN_NODE", 3: "MALFORMED_MSG",
    4: "UNSUPPORTED_QUERY", 5: "SESSION_EXPIRED", 6: "INTERNAL_ERROR",
    7: "VERSION_MISMATCH", 8: "NAME_IN_USE", 9: "PERMISSION_DENIED",
    10: "INVALID_PARAM",
}
(AUTH_FAILED, UNKNOWN_NODE, MALFORMED_MSG, UNSUPPORTED_QUERY,
 SESSION_EXPIRED, INTERNAL_ERROR, VERSION_MISMATCH, NAME_IN_USE,
 PERMISSION_DENIED, INVALID_PARAM) = range(1, 11)

# Estado del nodo según el servidor
STATE_NAMES = {0: "REGISTERED", 1: "ACTIVE", 2: "STALE", 3: "DISCONNECTED"}
PROFILE_NAMES = {0: "VIEWER", 1: "ADMIN"}

# Tipos de nodo / métrica (el canal de un nodo define su métrica)
NODE_TYPES = {0: "GENERIC", 1: "TEMPERATURE", 2: "HUMIDITY", 3: "MOTION",
              4: "CPU", 5: "BATTERY"}
METRIC_UNITS = {0: "", 1: "°C", 2: "%", 3: "", 4: "%", 5: "%"}

# Eventos
EV_THRESHOLD_EXCEEDED, EV_HARDWARE_FAULT, EV_TAMPER, EV_BACK_TO_NORMAL, \
    EV_MANUAL = 1, 2, 3, 4, 5
EVENT_NAMES = {1: "THRESHOLD_EXCEEDED", 2: "HARDWARE_FAULT", 3: "TAMPER",
               4: "BACK_TO_NORMAL", 5: "MANUAL"}
SEV_INFO, SEV_WARNING, SEV_CRITICAL = 0, 1, 2
SEVERITY_NAMES = {0: "INFO", 1: "WARNING", 2: "CRITICAL"}

# Backoff de reconexión y de resolución de nombres (maquinas-de-estado.md § 1)
BACKOFF = [1, 2, 4, 8, 16, 30]


class ProtocolError(Exception):
    """El par envió algo que no respeta el formato del protocolo."""


class ConnectionClosed(Exception):
    """El par cerró la conexión TCP."""


class ServerError(Exception):
    """El servidor respondió ERROR. Conserva el código y el texto."""

    def __init__(self, in_response_to, code, message):
        self.in_response_to = in_response_to
        self.code = code
        self.message = message
        super().__init__(f"{ERROR_NAMES.get(code, code)}: {message}")

    @property
    def code_name(self):
        return ERROR_NAMES.get(self.code, str(self.code))


# ------------------------------------------------------------- codificación

def header(msg_type, entity_id, payload_len):
    return struct.pack(HEADER_FMT, PROTO_VERSION, msg_type, entity_id,
                       payload_len)


def message(msg_type, entity_id, payload=b""):
    return header(msg_type, entity_id, len(payload)) + payload


def _lp8(data: bytes) -> bytes:
    """Campo con longitud de 1 byte al frente."""
    if len(data) > 255:
        raise ValueError("campo demasiado largo (máx. 255 bytes)")
    return struct.pack("!B", len(data)) + data


def encode_register(name, node_type, interval):
    payload = _lp8(name.encode("utf-8")) + struct.pack("!BH", node_type,
                                                      interval)
    return message(REGISTER, 0, payload)


def encode_event(node_id, code, severity, text, node_ts=None):
    if node_ts is None:
        node_ts = int(time.time())
    msg = text.encode("utf-8")[:MAX_PAYLOAD - 12]
    payload = struct.pack("!BBQH", code, severity, node_ts, len(msg)) + msg
    return message(EVENT, node_id, payload)


def encode_telemetry(node_id, metric_type, value, node_ts=None):
    if node_ts is None:
        node_ts = int(time.time())
    return message(TELEMETRY, node_id,
                   struct.pack("!BQf", metric_type, node_ts, value))


def encode_auth(username, password):
    payload = _lp8(username.encode("utf-8")) + _lp8(password.encode("utf-8"))
    return message(AUTH, 0, payload)


def encode_query(session_id, query_type, target=0, count=0):
    return message(QUERY, session_id,
                   struct.pack("!BIH", query_type, target, count))


# --------------------------------------------------------------- recepción

def recv_exact(sock, n):
    """Lee exactamente n bytes de un flujo TCP (un recv puede traer menos)."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionClosed("el servidor cerró la conexión")
        buf += chunk
    return bytes(buf)


def read_message(sock):
    """Devuelve (msg_type, entity_id, payload) del siguiente mensaje TCP."""
    hdr = recv_exact(sock, HEADER_SIZE)
    version, msg_type, entity_id, length = struct.unpack(HEADER_FMT, hdr)
    if version != PROTO_VERSION:
        raise ProtocolError(f"versión de protocolo no soportada: {version}")
    if length > MAX_PAYLOAD:
        raise ProtocolError(f"payload_length {length} excede MAX_PAYLOAD")
    payload = recv_exact(sock, length) if length else b""
    return msg_type, entity_id, payload


def read_response(sock, expected_in_response_to):
    """
    Lee la respuesta a una petición. Devuelve (entity_id, cuerpo) si es RESP,
    o lanza ServerError si es ERROR. Todo mensaje TCP recibe exactamente una
    respuesta (maquinas-de-estado.md § 4, regla 4).
    """
    msg_type, entity_id, payload = read_message(sock)
    if msg_type == RESP:
        if not payload:
            raise ProtocolError("RESP sin in_response_to")
        if payload[0] != expected_in_response_to:
            raise ProtocolError(
                f"RESP responde a {MSG_NAMES.get(payload[0], payload[0])}, "
                f"se esperaba {MSG_NAMES[expected_in_response_to]}")
        return entity_id, payload[1:]
    if msg_type == ERROR:
        if len(payload) < 3 or len(payload) != 3 + payload[2]:
            raise ProtocolError("ERROR mal formado")
        raise ServerError(payload[0], payload[1],
                          payload[3:].decode("utf-8", "replace"))
    raise ProtocolError(f"msg_type inesperado: {msg_type}")


# ------------------------------------------------- cuerpos de RESP(QUERY)

class _Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        size = struct.calcsize(fmt)
        if self.pos + size > len(self.data):
            raise ProtocolError("respuesta truncada")
        vals = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        return vals if len(vals) > 1 else vals[0]

    def raw(self, n):
        if self.pos + n > len(self.data):
            raise ProtocolError("respuesta truncada")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def done(self):
        if self.pos != len(self.data):
            raise ProtocolError("bytes sobrantes en la respuesta")


def parse_auth(body):
    r = _Reader(body)
    session_id, profile = r.take("!IB")
    r.done()
    return session_id, profile


def parse_status(body):
    r = _Reader(body)
    node_id, state, ts, value = r.take("!IBQf")
    r.done()
    return {"node_id": node_id, "state": state, "last_timestamp": ts,
            "last_value": value}


def parse_history(body):
    r = _Reader(body)
    count = r.take("!H")
    points = [r.take("!Qf") for _ in range(count)]
    r.done()
    return points          # [(timestamp, value)], del más reciente al más viejo


def parse_list(body):
    r = _Reader(body)
    count = r.take("!H")
    nodes = []
    for _ in range(count):
        node_id = r.take("!I")
        name = r.raw(r.take("!B")).decode("utf-8", "replace")
        state = r.take("!B")
        nodes.append({"node_id": node_id, "name": name, "state": state})
    r.done()
    return nodes


def parse_events(body):
    r = _Reader(body)
    count = r.take("!H")
    events = []
    for _ in range(count):
        server_ts, node_ts, code, sev = r.take("!QQBB")
        text = r.raw(r.take("!B")).decode("utf-8", "replace")
        events.append({"server_ts": server_ts, "node_ts": node_ts,
                       "code": code, "severity": sev, "message": text})
    r.done()
    return events


# --------------------------------------------- resolución y conexión TCP

def describe_gaierror(exc):
    """Distingue EAI_NONAME de EAI_AGAIN (despliegue-y-nombres.md, regla 4)."""
    if exc.errno == socket.EAI_NONAME:
        return f"EAI_NONAME: el nombre no existe ({exc.strerror})"
    if exc.errno == socket.EAI_AGAIN:
        return f"EAI_AGAIN: fallo temporal de DNS ({exc.strerror})"
    return f"fallo de resolución ({exc.strerror})"


def resolve(host, port, socktype, family=socket.AF_UNSPEC):
    """getaddrinfo(); lanza socket.gaierror si el nombre no resuelve."""
    return socket.getaddrinfo(host, port, family, socktype)


def connect_tcp(host, port, timeout=5.0):
    """
    Resuelve `host` y prueba cada dirección devuelta hasta que una conecte
    (no basta con la primera: puede ser una interfaz caída). Lanza
    socket.gaierror si la resolución falla u OSError si ninguna conecta.
    """
    last = None
    for family, socktype, proto, _, addr in resolve(host, port,
                                                    socket.SOCK_STREAM):
        s = socket.socket(family, socktype, proto)
        s.settimeout(timeout)
        try:
            s.connect(addr)
            s.settimeout(None)
            return s
        except OSError as exc:
            last = exc
            s.close()
    raise last or OSError(errno.EHOSTUNREACH, "ninguna dirección conectó")
