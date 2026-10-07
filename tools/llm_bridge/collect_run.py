#!/usr/bin/env python3
"""Archive one bridge run into the repository as evidence + a replay fixture.

    collect_run.py RUN_DIR META_JSON SUBAGENT_DIR DEST_DIR

RUN_DIR       what run_harness.sh produced (queue/, harness/out/, ...)
META_JSON     the run's metadata, written BEFORE the run (models, styles,
              ledger) and updated after it with persona ids and outcome
SUBAGENT_DIR  where the persona subagents' transcripts live
              (agent-<id>.jsonl)
DEST_DIR      e.g. tools/llm_bridge/runs/<label>

Copied verbatim: config_changes.json, the bridge log, every structured
request and every served response (with the persona's raw reply text), the
run's telemetry (hash-chained -- verify with naab-lang
--verify-telemetry-chain), agent transcript, stdout and stderr.

Not copied: governance-report.json / governance.sarif (re-derivable, ~110 KB
per run, identical in structure to telemetry's check results) and the raw
request bodies (requests/<n>.raw.json; the structured form keeps every field
the bridge reads, and the replay fixture regenerates the responses).

Derived: fixture.json (`bridge.py export-fixture`, replayable through
tests/helpers/agent_stub.py via run_harness.sh --replay) and one
personas/<persona>-<agent id>.jsonl per persona: each line is one request
the persona was sent (the director's message, verbatim, including any
private intervention direction) and the reply taken from it, with its
source (handback / pre-nudge-text) and any tool the persona used besides
the hand-back. The full subagent transcripts are ~0.5 MB each, nearly all
of it environment-injected context, so only this exchange is kept.
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bridge  # noqa: E402


def persona_exchange(path):
    """[(director_message, reply, source, other_tools)] for one transcript."""
    entries = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            m = e.get("message")
            if isinstance(m, dict) and m.get("role") in ("user", "assistant"):
                entries.append(m)
    directors = [bridge._user_text(m.get("content")) for m in entries
                 if m["role"] == "user"
                 and bridge.REQUEST_MARK in bridge._user_text(m.get("content"))]
    replies = bridge.transcript_replies(path)
    out = []
    for i, d in enumerate(directors):
        r = replies[i] if i < len(replies) else (None, [], "no-reply", d)
        out.append({"seq": i + 1, "director_message": d, "reply": r[0],
                    "reply_source": r[2], "persona_other_tool_uses": r[1]})
    return out


def main():
    run, meta_p, subdir, dest = sys.argv[1:5]
    if os.path.exists(dest) and os.listdir(dest):
        print("destination %s is not empty -- refusing" % dest, file=sys.stderr)
        return 2
    os.makedirs(dest, exist_ok=True)
    meta = json.load(open(meta_p, encoding="utf-8"))
    q = os.path.join(run, "queue")
    out = os.path.join(run, "harness", "out")

    with open(os.path.join(dest, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1)
    for src, name in [(os.path.join(run, "config_changes.json"), "config_changes.json"),
                      (os.path.join(q, "bridge_log.jsonl"), "bridge_log.jsonl"),
                      (os.path.join(q, "naab_exit"), "naab_exit"),
                      (os.path.join(out, "telemetry.jsonl"), "telemetry.jsonl"),
                      (os.path.join(out, "transcript.jsonl"), "transcript.jsonl"),
                      (os.path.join(out, "stdout.txt"), "stdout.txt"),
                      (os.path.join(out, "stderr.txt"), "stderr.txt")]:
        if os.path.exists(src):
            shutil.copy(src, os.path.join(dest, name))
    for sub, keep in [("requests", lambda f: f.endswith(".json") and not f.endswith(".raw.json")),
                      ("responses", lambda f: f.endswith(".json") or f.endswith(".reply.txt"))]:
        os.makedirs(os.path.join(dest, sub), exist_ok=True)
        for f in sorted(os.listdir(os.path.join(q, sub))):
            if keep(f):
                shutil.copy(os.path.join(q, sub, f), os.path.join(dest, sub, f))

    fx = subprocess.run([sys.executable, os.path.join(HERE, "bridge.py"),
                         "export-fixture", "--queue", q],
                        capture_output=True, text=True, check=True).stdout
    with open(os.path.join(dest, "fixture.json"), "w", encoding="utf-8") as f:
        f.write(fx)

    ids = dict(meta.get("persona_ids", {}))
    for i, ab in enumerate(meta.get("abandoned_personas", []), 1):
        ids["%s-abandoned%d" % (ab["persona"], i)] = ab["agent_id"]
    os.makedirs(os.path.join(dest, "personas"), exist_ok=True)
    flat = []
    for persona, aid in sorted(ids.items()):
        # A stateless persona (a fresh subagent per request) has a LIST.
        if isinstance(aid, list):
            flat += [("%s-r%d" % (persona, i), a) for i, a in enumerate(aid, 1)]
        else:
            flat.append((persona, aid))
    for persona, aid in flat:
        tp = os.path.join(subdir, "agent-%s.jsonl" % aid)
        if not os.path.exists(tp):
            print("WARNING: transcript missing for %s (%s)" % (persona, aid),
                  file=sys.stderr)
            continue
        with open(os.path.join(dest, "personas", "%s-%s.jsonl" % (persona, aid)),
                  "w", encoding="utf-8") as f:
            for row in persona_exchange(tp):
                f.write(json.dumps(row) + "\n")
    print("archived %s -> %s" % (run, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
