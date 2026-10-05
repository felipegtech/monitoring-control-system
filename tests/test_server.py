#!/usr/bin/env python3
"""
Pruebas de integración del servidor central (Fase 3: concurrencia,
resiliencia y pruebas).

Compila y arranca `server/server-stub` —el mismo servidor, con la
autenticación PAM sustituida por un verificador de prueba (contraseña
"test"; el usuario "admin" recibe el perfil ADMIN)— y lo ejercita por la
red, exactamente como lo haría un nodo o un cliente.

    python3 tests/test_server.py            # o: make -C server test
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "clients"))
import mcsproto as P  # noqa: E402

HOST = "localhost"     # se resuelve por nombre, igual que en producción


def free_port():
    with socket.socket() as s:
        s.bind(("", 0))
        return s.getsockname()[1]


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "server"),
                        "stub"], check=True)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.log_path = os.path.join(cls.tmp.name, "server.log")
        cls.port = free_port()
        cls.proc = subprocess.Popen(
            [os.path.join(ROOT, "server", "server-stub"), str(cls.port),
             cls.log_path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                socket.create_connection((HOST, cls.port), 0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("el servidor no arrancó")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(5)
        cls.tmp.cleanup()

    def setUp(self):
        self.assertIsNone(self.proc.poll(), "el servidor terminó")

    def tearDown(self):
        # Ninguna prueba debe poder tumbar el servidor.
        self.assertIsNone(self.proc.poll(), "el servidor se cayó")

    # ------------------------------------------------------------ utilidades

    def tcp(self):
        s = P.connect_tcp(HOST, self.port)
        s.settimeout(5)
        self.addCleanup(s.close)
        return s

    def register(self, name, interval=1, sock=None):
        s = sock or self.tcp()
        s.sendall(P.encode_register(name, 1, interval))
        _, body = P.read_response(s, P.REGISTER)
        return s, struct.unpack("!I", body)[0]

    def login(self, user="admin"):
        s = self.tcp()
        s.sendall(P.encode_auth(user, "test"))
        _, body = P.read_response(s, P.AUTH)
        sid, profile = P.parse_auth(body)
        return s, sid, profile

    def query(self, s, sid, qtype, target=0, count=0):
        s.sendall(P.encode_query(sid, qtype, target, count))
        _, body = P.read_response(s, P.QUERY)
        return body

    def telemetry(self, node_id, value, sock=None):
        u = sock or socket.socket(socket.AF_INET6 if ":" in self.udp_host()
                                  else socket.AF_INET, socket.SOCK_DGRAM)
        u.sendto(P.encode_telemetry(node_id, 1, value), self.udp_addr())
        if sock is None:
            u.close()

    def udp_addr(self):
        return P.resolve(HOST, self.port, socket.SOCK_DGRAM)[0][4]

    def udp_host(self):
        return self.udp_addr()[0]

    def assert_error(self, s, code, in_response_to=None):
        with self.assertRaises(P.ServerError) as cm:
            msg_type, _, payload = P.read_message(s)
            if msg_type == P.ERROR:
                raise P.ServerError(payload[0], payload[1],
                                    payload[3:].decode())
        self.assertEqual(cm.exception.code_name, P.ERROR_NAMES[code])
        if in_response_to is not None:
            self.assertEqual(cm.exception.in_response_to, in_response_to)

    def assert_closed(self, s):
        s.settimeout(3)
        try:
            self.assertEqual(s.recv(1), b"")
        except ConnectionResetError:
            pass

    def status(self, node_id):
        s, sid, _ = self.login()
        return P.parse_status(self.query(s, sid, P.Q_STATUS, node_id))

    # ---------------------------------------------------- registro de nodos

    def test_register_assigns_increasing_ids(self):
        _, a = self.register("reg-a")
        _, b = self.register("reg-b")
        self.assertGreater(a, 0)
        self.assertGreater(b, a)
        self.assertEqual(self.status(a)["state"], 0)   # REGISTERED

    def test_name_in_use_while_connected(self):
        self.register("dup")
        s = self.tcp()
        s.sendall(P.encode_register("dup", 1, 1))
        self.assert_error(s, P.NAME_IN_USE, P.REGISTER)

    def test_reregister_after_disconnect_gets_new_id(self):
        s, old = self.register("again")
        s.close()
        time.sleep(0.3)
        self.assertEqual(self.status(old)["state"], 3)   # DISCONNECTED
        _, new = self.register("again")
        self.assertNotEqual(old, new)
        c, sid, _ = self.login()
        c.sendall(P.encode_query(sid, P.Q_STATUS, old))
        self.assert_error(c, P.UNKNOWN_NODE)          # la entrada vieja se reinició

    def test_double_register_on_same_connection(self):
        s, _ = self.register("twice")
        s.sendall(P.encode_register("twice-2", 1, 1))
        self.assert_error(s, P.MALFORMED_MSG, P.REGISTER)

    # ------------------------------------------------------------ telemetría

    def test_telemetry_activates_and_feeds_history(self):
        _, nid = self.register("tele")
        for v in (1.5, 2.5, 3.5, 4.5, 5.5, 6.5):
            self.telemetry(nid, v)
            time.sleep(0.02)
        time.sleep(0.2)
        st = self.status(nid)
        self.assertEqual(st["state"], 1)              # ACTIVE
        self.assertAlmostEqual(st["last_value"], 6.5)
        s, sid, _ = self.login()
        hist = P.parse_history(self.query(s, sid, P.Q_HISTORY, nid, 0))
        self.assertEqual([v for _, v in hist], [6.5, 5.5, 4.5, 3.5, 2.5])

    def test_history_count_is_capped(self):
        _, nid = self.register("cap")
        for v in range(30):
            self.telemetry(nid, float(v))
        time.sleep(0.3)
        s, sid, _ = self.login()
        hist = P.parse_history(self.query(s, sid, P.Q_HISTORY, nid, 1000))
        self.assertLessEqual(len(hist), 20)
        self.assertGreaterEqual(len(hist), 5)

    def test_stale_and_recovery(self):
        _, nid = self.register("stale", interval=1)
        self.telemetry(nid, 10.0)
        time.sleep(0.2)
        self.assertEqual(self.status(nid)["state"], 1)
        time.sleep(4.5)                    # > 3 x interval + barrido
        self.assertEqual(self.status(nid)["state"], 2)   # STALE
        self.telemetry(nid, 11.0)
        time.sleep(0.2)
        self.assertEqual(self.status(nid)["state"], 1)   # vuelve a ACTIVE

    def test_telemetry_from_disconnected_node_is_dropped(self):
        s, nid = self.register("gone")
        self.telemetry(nid, 1.0)
        time.sleep(0.1)
        s.close()
        time.sleep(0.3)
        self.telemetry(nid, 99.0)
        time.sleep(0.2)
        st = self.status(nid)
        self.assertEqual(st["state"], 3)
        self.assertAlmostEqual(st["last_value"], 1.0)

    def test_garbage_udp_is_dropped(self):
        u = socket.socket(socket.AF_INET6 if ":" in self.udp_host()
                          else socket.AF_INET, socket.SOCK_DGRAM)
        addr = self.udp_addr()
        u.sendto(b"x", addr)                                    # < 10 bytes
        u.sendto(P.header(P.TELEMETRY, 1, 13) + b"\0" * 5, addr)  # truncado
        u.sendto(struct.pack("!BBII", 9, 3, 1, 13) + b"\0" * 13, addr)
        u.sendto(P.message(P.QUERY, 1, b"\0" * 7), addr)       # tipo erróneo
        u.sendto(P.encode_telemetry(424242, 1, 1.0), addr)      # id desconocido
        u.close()
        time.sleep(0.2)
        self.login()                       # sigue atendiendo

    # --------------------------------------------------------------- eventos

    def test_event_ack_and_query(self):
        s, nid = self.register("ev")
        s.sendall(P.encode_event(nid, P.EV_HARDWARE_FAULT, P.SEV_CRITICAL,
                                 "ventilador detenido"))
        P.read_response(s, P.EVENT)
        c, sid, _ = self.login()
        evs = P.parse_events(self.query(c, sid, P.Q_EVENTS, nid, 5))
        self.assertEqual(len(evs), 1)
        self.assertEqual(evs[0]["message"], "ventilador detenido")
        self.assertEqual(evs[0]["severity"], P.SEV_CRITICAL)

    def test_event_before_register_and_with_wrong_id(self):
        s = self.tcp()
        s.sendall(P.encode_event(7, 1, 2, "x"))
        self.assert_error(s, P.UNKNOWN_NODE, P.EVENT)
        s, nid = self.register("ev-wrong", sock=s)
        s.sendall(P.encode_event(nid + 1000, 1, 2, "x"))
        self.assert_error(s, P.UNKNOWN_NODE, P.EVENT)
        s.sendall(P.encode_event(nid, 1, 2, "ok"))
        P.read_response(s, P.EVENT)        # la conexión sigue viva

    def test_event_invalid_severity(self):
        s, nid = self.register("ev-sev")
        s.sendall(P.encode_event(nid, 1, 7, "x"))
        self.assert_error(s, P.INVALID_PARAM, P.EVENT)

    # ---------------------------------------------------- autenticación

    def test_auth_profiles(self):
        _, _, p_admin = self.login("admin")
        _, _, p_viewer = self.login("operador")
        self.assertEqual(p_admin, 1)
        self.assertEqual(p_viewer, 0)

    def test_three_failed_auth_close_connection(self):
        s = self.tcp()
        for _ in range(3):
            s.sendall(P.encode_auth("admin", "mala"))
            self.assert_error(s, P.AUTH_FAILED, P.AUTH)
        self.assert_closed(s)

    def test_retry_after_failed_auth(self):
        s = self.tcp()
        s.sendall(P.encode_auth("admin", "mala"))
        self.assert_error(s, P.AUTH_FAILED)
        s.sendall(P.encode_auth("admin", "test"))
        P.read_response(s, P.AUTH)

    def test_query_before_auth_closes(self):
        s = self.tcp()
        s.sendall(P.encode_query(0, P.Q_LIST_NODES))
        self.assert_error(s, P.SESSION_EXPIRED, P.QUERY)
        self.assert_closed(s)

    def test_wrong_session_id_keeps_connection(self):
        s, sid, _ = self.login()
        s.sendall(P.encode_query(sid + 1, P.Q_LIST_NODES))
        self.assert_error(s, P.SESSION_EXPIRED)
        P.parse_list(self.query(s, sid, P.Q_LIST_NODES))

    # ------------------------------------------------------------- consultas

    def test_unknown_node_and_unsupported_query(self):
        s, sid, _ = self.login()
        s.sendall(P.encode_query(sid, P.Q_STATUS, 999999))
        self.assert_error(s, P.UNKNOWN_NODE, P.QUERY)
        s.sendall(P.encode_query(sid, P.Q_HISTORY, 999999))
        self.assert_error(s, P.UNKNOWN_NODE, P.QUERY)
        s.sendall(P.encode_query(sid, 77))
        self.assert_error(s, P.UNSUPPORTED_QUERY, P.QUERY)

    def test_list_nodes_contains_registered(self):
        _, nid = self.register("listed")
        s, sid, _ = self.login()
        nodes = P.parse_list(self.query(s, sid, P.Q_LIST_NODES))
        self.assertIn(("listed", nid), [(n["name"], n["node_id"]) for n in nodes])

    def test_purge_requires_admin_and_disconnected(self):
        s, nid = self.register("purge-me")
        v, vsid, _ = self.login("operador")
        v.sendall(P.encode_query(vsid, P.Q_PURGE_NODE, nid))
        self.assert_error(v, P.PERMISSION_DENIED)
        a, asid, _ = self.login("admin")
        a.sendall(P.encode_query(asid, P.Q_PURGE_NODE, nid))
        self.assert_error(a, P.INVALID_PARAM)       # todavía conectado
        s.close()
        time.sleep(0.3)
        self.query(a, asid, P.Q_PURGE_NODE, nid)
        a.sendall(P.encode_query(asid, P.Q_STATUS, nid))
        self.assert_error(a, P.UNKNOWN_NODE)

    # ------------------------------------------------ mensajes mal formados

    def test_oversized_payload_length_closes(self):
        s = self.tcp()
        s.sendall(P.header(P.QUERY, 0, 99999))
        self.assert_error(s, P.MALFORMED_MSG)
        self.assert_closed(s)

    def test_version_mismatch_closes(self):
        s = self.tcp()
        s.sendall(struct.pack("!BBII", 2, P.AUTH, 0, 0))
        self.assert_error(s, P.VERSION_MISMATCH)
        self.assert_closed(s)

    def test_inconsistent_payload_keeps_connection(self):
        s = self.tcp()
        # REGISTER que declara un nombre de 50 bytes pero trae 3
        s.sendall(P.message(P.REGISTER, 0, b"\x32abc\x01\x00\x05"))
        self.assert_error(s, P.MALFORMED_MSG, P.REGISTER)
        self.register("after-malformed", sock=s)

    def test_query_with_wrong_length(self):
        s, sid, _ = self.login()
        s.sendall(P.message(P.QUERY, sid, b"\x01\x00"))
        self.assert_error(s, P.MALFORMED_MSG)
        P.parse_list(self.query(s, sid, P.Q_LIST_NODES))

    def test_invalid_msg_types_over_tcp(self):
        s = self.tcp()
        for t in (P.TELEMETRY, P.RESP, P.ERROR, 99):
            s.sendall(P.message(t, 0, b"abc"))
            self.assert_error(s, P.MALFORMED_MSG)

    def test_fragmented_and_coalesced_messages(self):
        s = self.tcp()
        data = P.encode_auth("admin", "test")
        for b in data:                      # un byte por send()
            s.send(bytes([b]))
            time.sleep(0.002)
        _, body = P.read_response(s, P.AUTH)
        sid, _ = P.parse_auth(body)
        q = P.encode_query(sid, P.Q_LIST_NODES)
        s.sendall(q + q + q)                # tres mensajes en un solo send()
        for _ in range(3):
            P.read_response(s, P.QUERY)

    def test_disconnect_mid_message(self):
        s = self.tcp()
        s.sendall(P.header(P.AUTH, 0, 20) + b"\x05ad")
        s.close()
        time.sleep(0.2)
        self.login()

    def test_idle_connection_without_first_message_is_harmless(self):
        idle = self.tcp()                   # conecta y no envía nada
        self.login()                        # otros siguen siendo atendidos
        idle.close()

    # ---------------------------------------------------------- concurrencia

    def test_concurrent_nodes_and_clients(self):
        errors = []
        node_ids = []
        lock = threading.Lock()

        def node(i):
            try:
                s, nid = self.register(f"conc-node-{i}")
                with lock:
                    node_ids.append(nid)
                for v in range(10):
                    self.telemetry(nid, float(v))
                    s.sendall(P.encode_event(nid, 1, 1, f"ev {v}"))
                    P.read_response(s, P.EVENT)
            except Exception as exc:     # noqa: BLE001
                errors.append(f"nodo {i}: {exc!r}")

        def client(i):
            try:
                s, sid, _ = self.login()
                for _ in range(20):
                    P.parse_list(self.query(s, sid, P.Q_LIST_NODES))
            except Exception as exc:     # noqa: BLE001
                errors.append(f"cliente {i}: {exc!r}")

        threads = [threading.Thread(target=node, args=(i,)) for i in range(15)]
        threads += [threading.Thread(target=client, args=(i,)) for i in range(30)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        self.assertEqual(errors, [])
        self.assertEqual(len(node_ids), 15)
        self.assertEqual(len(set(node_ids)), 15)

    # ----------------------------------------------------------------- log

    def test_log_has_peer_and_redacts_password(self):
        s = self.tcp()
        s.sendall(P.encode_auth("admin", "secreto-que-no-debe-aparecer"))
        self.assert_error(s, P.AUTH_FAILED)
        time.sleep(0.1)
        with open(self.log_path, encoding="utf-8") as f:
            text = f.read()
        self.assertNotIn("secreto-que-no-debe-aparecer", text)
        self.assertIn("password=<redacted>", text)
        local = s.getsockname()
        self.assertIn(f":{local[1]} RX AUTH", text)


class CommandLineTest(unittest.TestCase):
    BIN = os.path.join(ROOT, "server", "server-stub")

    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "server"),
                        "stub"], check=True)

    def test_usage_without_arguments(self):
        r = subprocess.run([self.BIN], capture_output=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn(b"uso", r.stderr)

    def test_invalid_port(self):
        r = subprocess.run([self.BIN, "70000", "/dev/null"], capture_output=True)
        self.assertEqual(r.returncode, 2)

    def test_unwritable_log(self):
        r = subprocess.run([self.BIN, str(free_port()), "/no/existe/x.log"],
                           capture_output=True)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
