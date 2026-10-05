#!/usr/bin/env python3
"""Patch a COPY of agent_harness's govern.json for a bridge run.

    patch_config.py PORT [DEVIATION] < govern.json > patched.json 2> changes.json

DEVIATION (optional) names ONE further, deliberate change away from the
shipped governance, for runs that cannot otherwise reach the stages under
study. Each is recorded with "deviation": true. Runs made with one are a
separate set and must never be pooled with shipped-config runs.

  worker-tool-budget-8   agents.worker.max_tool_calls_per_turn 4 -> 8.
                         Why: under the shipped 4, every clean run (worker
                         = haiku, opus, sonnet) ended at audit step 1 --
                         list_files + read_file x3 = 4 calls exhausts the
                         budget, the loop exits with no final model turn,
                         and the harness dies on the empty answer
                         (runs/A1, B1, C1). Nothing after step 1 could be
                         observed. Chosen by the user over "shipped only".

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
    deviation = sys.argv[2] if len(sys.argv) > 2 else ""
    cfg = json.load(sys.stdin)
    changes = []

    def setv(path, obj, key, new, dev=False):
        changes.append({"path": path, "before": obj.get(key, "<absent>"),
                        "after": new, "deviation": dev})
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

    if deviation == "worker-tool-budget-8":
        setv("agents.worker.max_tool_calls_per_turn", cfg["agents"]["worker"],
             "max_tool_calls_per_turn", 8, dev=True)
    elif deviation:
        raise SystemExit("unknown deviation: " + deviation)

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
