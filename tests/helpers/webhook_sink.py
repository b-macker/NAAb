#!/usr/bin/env python3
"""Loopback webhook sink for telemetry-forwarding tests (stdlib only).

    webhook_sink.py PORTFILE OUTFILE [--hang]

Binds 127.0.0.1 on a free port, writes the port to PORTFILE, and appends every
event of every POSTed JSON array to OUTFILE, one compact JSON line each, in
arrival order -- the same one-line-per-event form the telemetry file uses, so
a test can compare the two directly. --hang accepts each request, reads it,
and never answers: a webhook that stalls, for the drain-deadline arms.

Writes ASCII only (json.dumps default) and flushes per request, so a reader
never sees half a batch.
"""
import http.server
import json
import sys
import threading
import time


def main():
    port_file, out_file = sys.argv[1], sys.argv[2]
    hang = "--hang" in sys.argv[3:]
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n)
            if hang:
                time.sleep(3600)
                return
            try:
                events = json.loads(body.decode("utf-8", "replace"))
            except ValueError:
                events = [{"unparsed": body.decode("utf-8", "replace")}]
            if not isinstance(events, list):
                events = [events]
            with lock, open(out_file, "a", encoding="utf-8", errors="replace") as fh:
                for ev in events:
                    fh.write(json.dumps(ev, separators=(",", ":"), sort_keys=True) + "\n")
                fh.flush()
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_message(self, *args):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    with open(port_file, "w", encoding="ascii") as fh:
        fh.write(str(srv.server_address[1]))
    srv.serve_forever()


if __name__ == "__main__":
    main()
