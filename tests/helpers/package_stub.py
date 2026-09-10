#!/usr/bin/env python3
"""Loopback GitHub-API stub for the package manager tests.

The package manager talks to https://api.github.com with a hardcoded scheme and
host. NAAB_PKG_API_BASE redirects it at a loopback address (and ONLY a loopback
address) so a test can serve its own tarballs without a network, a TLS
certificate, or root -- reaching the real endpoint over TLS would need a
trusted MITM CA, which no CI runner should be asked to install.

Routes, matching the three URLs package_manager.cpp builds:
    GET /repos/<owner>/<repo>/tarball/<ref>   -> ./<owner>-<repo>-<ref>.tar.gz
    GET /repos/<owner>/<repo>/releases/latest -> {"tag_name": <newest served>}
    GET /repos/<owner>/<repo>/tags            -> []

Anything with no file on disk is a 404, which is what the manager sees when a
tag does not exist -- the fallback that retries the "v" prefix depends on it.

Output is ASCII and written as bytes: a test that parses this log must not be
at the mercy of the locale's stdout encoding (see tests/helpers/encoding_controls.sh).
"""
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

SERVE_DIR = "."


def log(msg):
    sys.stdout.buffer.write((msg + "\n").encode("ascii", "replace"))
    sys.stdout.flush()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # silence the default stderr spew
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        parts = [p for p in path.split("/") if p]
        if len(parts) == 5 and parts[0] == "repos" and parts[3] == "tarball":
            owner, repo, ref = parts[1], parts[2], parts[4]
            name = "%s-%s-%s.tar.gz" % (owner, repo, ref)
            full = os.path.join(SERVE_DIR, name)
            if os.path.isfile(full):
                with open(full, "rb") as fh:
                    body = fh.read()
                log("SERVE %s (%d bytes)" % (name, len(body)))
                self._send(200, body, "application/gzip")
                return
            log("MISS %s" % name)
            self._send(404, b'{"message":"Not Found"}', "application/json")
            return

        if len(parts) == 5 and parts[3] == "releases" and parts[4] == "latest":
            owner, repo = parts[1], parts[2]
            prefix = "%s-%s-" % (owner, repo)
            tags = sorted(
                f[len(prefix):-len(".tar.gz")]
                for f in os.listdir(SERVE_DIR)
                if f.startswith(prefix) and f.endswith(".tar.gz")
            )
            if not tags:
                self._send(404, b'{"message":"Not Found"}', "application/json")
                return
            body = ('{"tag_name": "%s"}' % tags[-1]).encode("ascii")
            log("LATEST %s/%s -> %s" % (owner, repo, tags[-1]))
            self._send(200, body, "application/json")
            return

        if len(parts) == 4 and parts[3] == "tags":
            self._send(200, b"[]", "application/json")
            return

        log("UNROUTED %s" % path)
        self._send(404, b'{"message":"Not Found"}', "application/json")


def main():
    global SERVE_DIR
    port = int(sys.argv[1])
    SERVE_DIR = sys.argv[2]
    server = HTTPServer(("127.0.0.1", port), Handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    log("READY %d" % port)
    try:
        while True:
            t.join(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
