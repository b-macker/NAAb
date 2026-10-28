#!/usr/bin/env python3
"""Drive naab-lsp over stdio for one document and print what it publishes.

    lsp_diag_probe.py NAAB_LSP FILE

Sends initialize, initialized, didOpen (FILE's text), shutdown, exit -- the
Content-Length framing an editor uses -- and prints, one JSON line each:
  {"init": true}                       when the initialize response arrives
  {"code": ..., "line": ..., "message": ...}
                                       per diagnostic whose source is
                                       "naab-governance"
Nothing else. Stdlib only; ASCII output (json.dumps default).
"""
import json
import os
import subprocess
import sys


def frame(obj):
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def main():
    lsp, path = sys.argv[1], os.path.abspath(sys.argv[2])
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    uri = "file://" + path
    stdin = (frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
             + frame({"jsonrpc": "2.0", "method": "initialized", "params": {}})
             + frame({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                      "params": {"textDocument": {"uri": uri, "languageId": "naab",
                                                  "version": 1, "text": text}}})
             + frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown"})
             + frame({"jsonrpc": "2.0", "method": "exit"}))
    env = dict(os.environ, NAAB_LSP_LOG_LEVEL="ERROR")
    out = subprocess.run([lsp], input=stdin, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env, timeout=60).stdout
    pos = 0
    while True:
        start = out.find(b"Content-Length: ", pos)
        if start < 0:
            break
        sep = out.find(b"\r\n\r\n", start)
        if sep < 0:
            break
        length = int(out[start + 16:sep])
        body = out[sep + 4:sep + 4 + length]
        pos = sep + 4 + length
        msg = json.loads(body)
        if msg.get("id") == 1 and "capabilities" in msg.get("result", {}):
            print(json.dumps({"init": True}))
        if msg.get("method") == "textDocument/publishDiagnostics":
            for d in msg["params"]["diagnostics"]:
                if d.get("source") == "naab-governance":
                    print(json.dumps({"code": d.get("code"),
                                      "line": d["range"]["start"]["line"],
                                      "message": d.get("message")}))


if __name__ == "__main__":
    main()
