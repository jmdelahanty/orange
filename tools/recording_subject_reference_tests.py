#!/usr/bin/env python3
"""Start a local stand-in for the MetaZebrobot API and run
recording_subject_reference_tests against it, so Orange's minimal HTTP client
is exercised for 200 JSON, 404 structured error, a stalled response (timeout)
and a refused connection.

Usage: recording_subject_reference_tests.py <path-to-recording_subject_reference_tests>
"""
from __future__ import annotations

import json
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

DISH = {"schema_version": 2, "dish_id": "19220_1", "dish_uuid": "28c29cc6-a1ef-4382-9dc5-414fbb445d92",
        "revision": 1, "updated_at": "2026-10-02 16:28:20", "genotype": "wt", "dof": "2026-09-20",
        "fish_count": 12, "species": "zebrafish"}
FISH = {"items": [{"fish_id": "19220_1_f1", "dish_id": "19220_1", "dish_uuid": DISH["dish_uuid"],
                   "subject_label": "f1", "current_unit_id": None, "revision": 3, "updated_at": "2026-10-03 09:00:00"}]}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _json(self, status: int, payload: dict, chunked: bool = False) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            half = len(body) // 2
            for part in (body[:half], body[half:]):
                self.wfile.write(f"{len(part):x}\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        if self.path == "/dishes/19220_1/citrus-snapshot":
            self._json(200, DISH)
        elif self.path == "/dishes/19220_1/fish":
            self._json(200, FISH, chunked=True)  # exercises chunked decoding
        elif self.path.startswith("/dishes/") and self.path.endswith("/citrus-snapshot"):
            dish_id = self.path.split("/")[2]
            self._json(404, {"detail": {"error": "dish_not_found", "dish_id": dish_id}})
        elif self.path == "/slow":
            time.sleep(2.0)  # longer than the client's timeout
            try:
                self._json(200, {})
            except (BrokenPipeError, ConnectionResetError):
                pass  # the client gave up, as intended
        else:
            self._json(404, {"detail": {"error": "not_found"}})

    def log_message(self, *args):  # quiet
        pass


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    server = HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"http://127.0.0.1:{server.server_port}"
        result = subprocess.run([sys.argv[1], url], capture_output=True, text=True, timeout=60)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode
    finally:
        server.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
