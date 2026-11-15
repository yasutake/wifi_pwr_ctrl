import hashlib
import json
import os
import socketserver
import struct
import subprocess
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def sha(data):
    return hashlib.sha256(data).digest()


def aes(key, iv, data, encrypt):
    cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
    if encrypt:
        pad = padding.PKCS7(128).padder()
        data = pad.update(data) + pad.finalize()
        ctx = cipher.encryptor()
        return ctx.update(data) + ctx.finalize()
    ctx = cipher.decryptor()
    data = ctx.update(data) + ctx.finalize()
    unpad = padding.PKCS7(128).unpadder()
    return unpad.update(data) + unpad.finalize()


class Tapo(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        data = self.rfile.read(int(self.headers["Content-Length"]))
        server = self.server
        if self.path == "/app/handshake1":
            server.local, server.remote = data, b"r" * 16
            server.auth = sha(hashlib.sha1(b"user").digest() + hashlib.sha1(b"pass").digest())
            reply = server.remote + sha(server.local + server.remote + server.auth)
        elif self.path == "/app/handshake2":
            assert self.headers["Cookie"] == "TP_SESSIONID=session"
            assert data == sha(server.remote + server.local + server.auth)
            reply = b""
        else:
            assert self.headers["Cookie"] == "TP_SESSIONID=session"
            secret = server.local + server.remote + server.auth
            key = sha(b"lsk" + secret)[:16]
            iv = sha(b"iv" + secret)
            seq = int(self.path.split("=")[1])
            seq_bytes = struct.pack(">i", seq)
            iv_bytes = iv[:12] + seq_bytes
            sig = sha(b"ldk" + secret)[:28]
            assert data[:32] == sha(sig + seq_bytes + data[32:])
            request = json.loads(aes(key, iv_bytes, data[32:], False))
            server.requests.append(request)
            method = request["method"]
            if method == "get_device_info":
                result = {"device_on": True}
            elif method == "get_energy_usage":
                result = {"current_power": 1234, "today_energy": 456, "month_energy": 7890}
            elif method == "get_child_device_list":
                index = request["params"]["start_index"]
                result = {"start_index": index, "sum": 2, "child_device_list": [
                    {"device_id": f"child{index}", "device_on": bool(index), "position": index + 1}
                ]}
            elif method == "control_child":
                result = {"responseData": {"error_code": server.child_error}}
            else:
                result = {}
            response = json.dumps({"error_code": 0, "result": result}).encode()
            ciphertext = aes(key, iv_bytes, response, True)
            reply = sha(sig + seq_bytes + ciphertext) + ciphertext
            if server.bad_signature:
                reply = b"\0" * 32 + ciphertext
        self.send_response(200)
        if self.path == "/app/handshake1":
            self.send_header("Set-Cookie", "TP_SESSIONID=session; Path=/")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def xor(data, encrypt):
    key = 171
    result = bytearray()
    for byte in data:
        out = byte ^ key
        key = out if encrypt else byte
        result.append(out)
    return bytes(result)


def read_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise RuntimeError("unexpected EOF")
        data += chunk
    return data


class Kasa(socketserver.BaseRequestHandler):
    def handle(self):
        length = struct.unpack(">I", read_exact(self.request, 4))[0]
        request = json.loads(xor(read_exact(self.request, length), False))
        self.server.requests.append(request)
        system = request["system"]
        if "get_sysinfo" in system:
            response = {"system": {"get_sysinfo": {"err_code": 0, "relay_state": 1}}}
        else:
            response = {"system": {"set_relay_state": {"err_code": self.server.error}}}
        data = xor(json.dumps(response).encode(), True)
        # Fragment the header and payload to exercise short reads.
        wire = struct.pack(">I", len(data)) + data
        for b in wire:
            self.request.sendall(bytes([b]))


class CLITest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tapo = HTTPServer(("127.0.0.1", 0), Tapo)
        cls.kasa = socketserver.TCPServer(("127.0.0.1", 0), Kasa)
        for server in (cls.tapo, cls.kasa):
            server.requests = []
            threading.Thread(target=server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        for server in (cls.tapo, cls.kasa):
            server.shutdown()
            server.server_close()

    def setUp(self):
        self.tapo.requests.clear()
        self.kasa.requests.clear()
        self.tapo.bad_signature = False
        self.tapo.child_error = 0
        self.kasa.error = 0

    def run_cli(self, model, command, *args, env=None):
        server = self.kasa if model == "HS105" else self.tapo
        environment = dict(os.environ, TAPO_USERNAME="user", TAPO_PASSWORD="pass")
        if env:
            environment.update(env)
        return subprocess.run(
            ["./wifi-pwr-ctrl", "--model", model, "--ip", "127.0.0.1",
             "--port", str(server.server_address[1]), *args, command],
            capture_output=True, text=True, env=environment, timeout=10,
        )

    def test_plug_status_energy_and_control(self):
        result = self.run_cli("P110M", "energy")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout),
                         {"power_w": 1.234, "today_kwh": .456, "month_kwh": 7.89})
        result = self.run_cli("P105", "status")
        self.assertEqual(json.loads(result.stdout), {"device_on": True})
        result = self.run_cli("P105", "off")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.tapo.requests[-1]["params"], {"device_on": False})

    def test_p300_pagination_and_child_control(self):
        result = self.run_cli("P300", "status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([x["device_id"] for x in json.loads(result.stdout)], ["child0", "child1"])
        result = self.run_cli("P300", "on", "--child", "child1")
        self.assertEqual(result.returncode, 0, result.stderr)
        params = self.tapo.requests[-1]["params"]
        self.assertEqual(params["device_id"], "child1")
        self.assertEqual(params["requestData"], {"method": "set_device_info", "params": {"device_on": True}})
        self.tapo.child_error = -1
        self.assertNotEqual(self.run_cli("P300", "off", "--child", "child1").returncode, 0)
        self.assertNotEqual(self.run_cli("P300", "status", "--child", "missing").returncode, 0)
        self.assertEqual(self.run_cli("P300", "on").returncode, 2)

    def test_kasa_and_unsupported_energy(self):
        result = self.run_cli("HS105", "status")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"device_on": True})
        self.assertEqual(self.run_cli("HS105", "on").returncode, 0)
        self.assertEqual(self.kasa.requests[-1]["system"]["set_relay_state"]["state"], 1)
        self.kasa.error = -1
        self.assertNotEqual(self.run_cli("HS105", "off").returncode, 0)
        self.assertEqual(self.run_cli("HS105", "energy").returncode, 2)

    def test_klap_authentication_and_signature_errors(self):
        result = self.run_cli("P105", "status", env={"TAPO_PASSWORD": "wrong"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("authentication failed", result.stderr)
        self.tapo.bad_signature = True
        result = self.run_cli("P105", "status")
        self.assertEqual(result.returncode, 1)
        self.assertIn("signature", result.stderr)


if __name__ == "__main__":
    unittest.main()
