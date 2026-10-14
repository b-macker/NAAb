#!/usr/bin/env python3
"""A stdio MCP server stub for the guidance MCP-source tests (python3 stdlib only).

Usage: mcp_guidance_stub.py <behaviour.json>

The behaviour file is written by the test (operator-side, like govern.json):

  log            path; every line received is appended as {"recv": <message>}
                 and every environment probe as {"env": {...}}
  counter        path; one line appended per session (counts server starts)
  env_probe      [names]; their values (or null) are logged at start-up
  version        protocolVersion to answer initialize with (default: the
                 client's own)
  no_resources   true: initialize advertises no resources capability
  pages          list of pages; each a list of {uri, name[, title]}. The
                 list call returns them one page per call, with nextCursor
  texts          {uri: text}; resources/read answers with this text
  blobs          [uri]; resources/read answers with a blob only (no text)
  fail_read      [uri]; resources/read answers with a JSON-RPC error
  hang_on        a method name: never answer it
  exit_on        a method name: exit without answering it
  ask_first      true: before answering resources/list, send the client a
                 ping and a sampling/createMessage request and log the replies
  spawn_child    path; start a long-lived grandchild and write its pid there
  mode_file      path; if it exists and reads "crash", exit at start-up

Messages are newline-delimited JSON-RPC (the MCP stdio transport).
"""
import json
import os
import subprocess
import sys

cfg = json.load(open(sys.argv[1], encoding="utf-8"))
LOG = cfg.get("log")


def log(obj):
    if LOG:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(obj) + "\n")


if cfg.get("mode_file") and os.path.exists(cfg["mode_file"]):
    if open(cfg["mode_file"], encoding="utf-8").read().strip() == "crash":
        sys.exit(3)
if cfg.get("counter"):
    with open(cfg["counter"], "a", encoding="utf-8") as f:
        f.write("session\n")
if cfg.get("env_probe"):
    log({"env": {k: os.environ.get(k) for k in cfg["env_probe"]}})
if cfg.get("spawn_child"):
    p = subprocess.Popen(["sleep", "60"])
    with open(cfg["spawn_child"], "w", encoding="utf-8") as f:
        f.write(str(p.pid))


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def read_msg():
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    msg = json.loads(line)
    log({"recv": msg})
    return msg


pages = cfg.get("pages", [])
texts = cfg.get("texts", {})
asked = False

while True:
    msg = read_msg()
    method = msg.get("method")
    mid = msg.get("id")
    if method is None or mid is None:
        continue  # a notification, or a reply to one of ours (already logged)
    if method == cfg.get("exit_on"):
        sys.exit(0)
    if method == cfg.get("hang_on"):
        continue
    if method == "initialize":
        caps = {} if cfg.get("no_resources") else {"resources": {}}
        ver = cfg.get("version") or msg.get("params", {}).get("protocolVersion")
        send({"jsonrpc": "2.0", "id": mid, "result": {
            "protocolVersion": ver, "capabilities": caps,
            "serverInfo": {"name": "guidance-stub", "version": "1"}}})
    elif method == "resources/list":
        if cfg.get("ask_first") and not asked:
            asked = True
            send({"jsonrpc": "2.0", "id": "srv-ping", "method": "ping"})
            read_msg()
            send({"jsonrpc": "2.0", "id": "srv-sample", "method": "sampling/createMessage",
                  "params": {"messages": [{"role": "user", "content": {"type": "text", "text": "hi"}}],
                             "maxTokens": 10}})
            read_msg()
        cursor = msg.get("params", {}).get("cursor")
        idx = int(cursor) if cursor else 0
        result = {"resources": pages[idx] if idx < len(pages) else []}
        if idx + 1 < len(pages):
            result["nextCursor"] = str(idx + 1)
        send({"jsonrpc": "2.0", "id": mid, "result": result})
    elif method == "resources/read":
        uri = msg.get("params", {}).get("uri")
        if uri in cfg.get("fail_read", []):
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32002, "message": "Resource not found"}})
        elif uri in cfg.get("blobs", []):
            send({"jsonrpc": "2.0", "id": mid, "result": {"contents": [
                {"uri": uri, "mimeType": "application/octet-stream", "blob": "AAEC"}]}})
        else:
            send({"jsonrpc": "2.0", "id": mid, "result": {"contents": [
                {"uri": uri, "mimeType": "text/markdown", "text": texts.get(uri, "")}]}})
    else:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "Method not found"}})
