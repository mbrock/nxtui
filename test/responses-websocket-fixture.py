#!/usr/bin/env python3
"""Offline OpenAI Responses wire-contract fixture over authenticated WSS."""
import base64
import hashlib
import json
from pathlib import Path
from runpy import run_path
import ssl
import subprocess
import sys
import tempfile
import threading

ws = run_path(str(Path(__file__).with_name("websocket-fixture.py")))
frame, Server, Handler, certificate = (ws[k] for k in ("frame", "Server", "Handler", "certificate"))


class ResponsesHandler(Handler):
    def handle(self):
        self.connection.settimeout(10)
        try:
            self.session()
        except Exception as exc:
            self.server.errors.append(repr(exc))

    def event(self, payload):
        self.connection.sendall(frame(1, json.dumps(payload).encode()))

    def create(self):
        opcode, raw = self.client_frame()
        assert opcode == 1, "response.create must be text"
        data = json.loads(raw)
        assert data["type"] == "response.create" and data["model"] == "fixture-model"
        assert data["store"] is False
        assert not {"stream", "background", "api_key"}.intersection(data), "HTTP-only/secret fields"
        assert "fixture-key" not in raw.decode(), "credential in event"
        return data

    def complete(self, response_id, output):
        self.event({"type": "response.completed", "response": {
            "id": response_id, "status": "completed", "output": output}})

    def session(self):
        method, path, version = self.rfile.readline(4096).decode().split()
        assert method == "GET" and version == "HTTP/1.1"
        headers = {}
        while True:
            line = self.rfile.readline(4096)
            if line == b"\r\n":
                break
            assert line, "truncated Upgrade"
            name, value = line.decode().split(":", 1)
            assert name.lower() not in headers, "duplicate Upgrade header"
            headers[name.lower()] = value.strip()
        assert headers["authorization"] == "Bearer fixture-key"
        assert headers["upgrade"].lower() == "websocket"
        assert headers["connection"].lower() == "upgrade"
        assert headers["sec-websocket-version"] == "13"
        accept = base64.b64encode(hashlib.sha1(
            (headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.connection.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
        first = self.create()
        assert first["input"] == "inspect" and "previous_response_id" not in first
        if path == "/agent":
            assert first["max_output_tokens"] == 731
            assert first["include"] == ["reasoning.encrypted_content"]
            assert first["reasoning"] == {"effort": "low", "summary": "auto"}
            assert first["tools"][0]["parameters"] == {"type": "object"}
            self.connection.sendall(frame(9, b"ping") + frame(10, b"pong"))
            assert self.client_frame() == (10, b"ping")
            self.event({"type": "response.function_call_arguments.delta", "delta": "{"})
            for turn in (1, 2):
                self.complete(f"resp_tool_{turn}", [
                    {"type": "reasoning", "encrypted_content": "opaque", "summary": []},
                    {"type": "function_call", "call_id": f"call_{turn}", "name": "missing", "arguments": "{}"}])
                continuation = self.create()
                assert continuation["previous_response_id"] == f"resp_tool_{turn}"
                assert continuation["tools"] == first["tools"] and continuation["reasoning"] == first["reasoning"]
                assert continuation["include"] == first["include"] and continuation["max_output_tokens"] == 731
                assert len(continuation["input"]) == 1, "context was replayed"
                result = continuation["input"][0]
                assert result["type"] == "function_call_output" and result["call_id"] == f"call_{turn}"
                assert "unknown tool" in result["output"]
            self.event({"type": "response.output_text.delta", "delta": "first "})
            self.event({"type": "response.refusal.delta", "delta": "answer"})
            message = {"type": "message", "phase": "final_answer", "content": []}
            self.complete("resp_answer", [message])
            fresh = self.create()
            assert fresh["input"] == "fresh chain" and "previous_response_id" not in fresh
            self.complete("resp_fresh", [message])
            assert self.rfile.read(1) == b"", "unexpected request after fresh chain"
            self.server.seen.add(path)
            return
        if path == "/stall":
            self.event({"type": "response.output_text.delta", "delta": "parked"})
        elif path == "/close":
            self.connection.sendall(frame(8, b"\x03\xe8"))
            assert self.client_frame() == (8, b"\x03\xe8")
        elif path == "/binary":
            self.connection.sendall(frame(2, b"{}"))
        elif path == "/malformed":
            self.connection.sendall(frame(1, b'{"type":"response.created"} {}'))
        elif path == "/missing-type":
            self.event({"response": {}})
        elif path == "/observer":
            self.event({"type": "response.output_text.delta", "delta": "discarded"})
        elif path == "/error":
            self.event({"type": "error", "status": 400, "error": {
                "code": "previous_response_not_found", "message": "cached response gone"}})
        else:
            assert path in ("/failed", "/incomplete")
            self.event({"type": "response." + path[1:], "response": {"status": path[1:]}})
        assert self.rfile.read(1) == b"", "failure retried or session not disconnected"
        self.server.seen.add(path)


def main():
    with tempfile.TemporaryDirectory(prefix="nxt-responses-ws-") as temp:
        directory = Path(temp)
        certificate(directory, "server", "localhost")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(directory / "server.pem", directory / "server.key")
        server = Server(context)
        server.RequestHandlerClass = ResponsesHandler
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            result = subprocess.run([str(Path(sys.argv[1]).resolve()),
                                     f"wss://localhost:{server.server_address[1]}",
                                     str(directory / "server.pem")], timeout=90).returncode
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        expected = {"/agent", "/failed", "/incomplete", "/error", "/close", "/binary",
                    "/malformed", "/missing-type", "/observer", "/stall"}
        if server.seen != expected:
            server.errors.append("missing completed cases: " + repr(expected - server.seen))
        for error in server.errors:
            print("FIXTURE FAIL:", error)
        print(f"RESPONSES FIXTURE: {len(server.errors)} server assertion failures")
        return result or bool(server.errors)


if __name__ == "__main__":
    raise SystemExit(main())
