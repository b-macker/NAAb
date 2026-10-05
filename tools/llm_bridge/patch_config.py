#!/usr/bin/env python3
"""Patch a COPY of agent_harness's govern.json for a bridge run.

    patch_config.py PORT < govern.json > patched.json 2> changes.json

Reads the config from stdin (bytes, not a path -- a native helper need not
share the shell's path vocabulary). Writes the patched config to stdout and
the list of every change ({path, before, after}) to stderr as JSON. Only the
settings named in run_harness.sh's header are touched; everything else is the
shipped governance. Fails (non-zero, traceback) if telemetry or the transcript
is not enabled in the shipped config, rather than enabling it silently.
"""
import json
import sys


def main():
    port = sys.argv[1]
    cfg = json.load(sys.stdin)
    changes = []

    def setv(path, obj, key, new):
        changes.append({"path": path, "before": obj.get(key, "<absent>"),
                        "after": new})
        obj[key] = new

    for name, a in cfg["agents"].items():
        setv("agents.%s.api_base" % name, a, "api_base",
             "http://127.0.0.1:" + port)
        if a.get("standing_lease_seconds", 0) > 0:
            setv("agents.%s.standing_lease_seconds" % name, a,
                 "standing_lease_seconds", 86400)
    ad = cfg["agent_dispatch"]
    setv("agent_dispatch.default_timeout_seconds", ad,
         "default_timeout_seconds", 7200)
    setv("agent_dispatch.hard_stop.max_agent_time_ms", ad["hard_stop"],
         "max_agent_time_ms", 21600000)
    setv("runtime.timeout", cfg["runtime"], "timeout", 21600)
    setv("limits.timeout.global", cfg["limits"]["timeout"], "global", 21600)

    tel = cfg.get("telemetry", {})
    assert tel.get("enabled") is True, "telemetry not enabled in shipped config"
    assert tel.get("transcript", {}).get("enabled") is True, \
        "transcript not enabled in shipped config"
    assert tel.get("transcript", {}).get("agents") == [], \
        "transcript is filtered to some agents"

    json.dump(cfg, sys.stdout, indent=2)
    sys.stderr.write(json.dumps(changes, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
