#!/usr/bin/env python3
"""Build a v3 response fixture from REAL repository text, code or prose.

    python3 gen_modality_fixture.py code  <out.json> [n]
    python3 gen_modality_fixture.py prose <out.json> [n]

WHY THE TEXT IS NOT WRITTEN HERE

C1d says a compliant agent doing ordinary coding work floors its coherence in
six turns, driven by semantic_stability and entity_consistency -- two signals
that read ONLY the response stream. The obvious control is to ask whether prose
responses do the same. The trap is that both signals measure vocabulary overlap
between consecutive responses, so an author who writes the two arms is choosing
the quantity being measured, and the experiment returns whatever they expected.

So neither arm is authored. Both are lifted verbatim from text that already
exists in this repository and was written for other purposes: the code arm from
src/*.cpp, the prose arm from docs/*.md. The repo supplies its own controls.

Chunks are normalised to the same character budget, because the stub derives
output_tokens from content length and response length feeds response_quality and
persona_fingerprint. Without that, a difference between arms could be length
rather than modality.
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHUNK_CHARS = 1200


MIN_CHARS = 1000


def _chunks(text, want, sep):
    """Split into ~CHUNK_CHARS pieces at `sep` boundaries, never mid-unit.

    Short tail pieces are DISCARDED rather than emitted. The stub derives
    output_tokens from content length, so a stray 37-character chunk is not just
    untidy -- it is a different response length, and response length feeds
    response_quality and persona_fingerprint. An arm containing one would differ
    from the other arm by length as well as by modality, and the comparison
    would no longer isolate anything.
    """
    out, buf = [], ""
    for unit in text.split(sep):
        buf = buf + sep + unit if buf else unit
        if len(buf) >= CHUNK_CHARS:
            out.append(buf[:CHUNK_CHARS])
            buf = ""
            if len(out) >= want:
                return out
    if len(buf.strip()) >= MIN_CHARS:
        out.append(buf)
    return out


def _largest(paths):
    best, best_n = None, -1
    for p in paths:
        try:
            n = os.path.getsize(p)
        except OSError:
            continue
        if n > best_n:
            best, best_n = p, n
    return best


def code_chunks(want):
    # ONE file, consecutive chunks -- not a walk over many files.
    #
    # The first version of this walked src/ and concatenated slices of unrelated
    # translation units. Both arms floored at turn 3-4 against a live reference
    # of 7-8, i.e. the stimulus was harsher than the thing it models, and "both
    # arms floor" is what ANY sufficiently heterogeneous stream produces
    # regardless of modality. A real agent's turns are successive work on ONE
    # task, so consecutive responses are topically related; consecutive slices
    # of different files share almost nothing by construction. Taking a single
    # large file reproduces the relatedness the live runs actually had.
    src = os.path.join(REPO, "src")
    files = []
    for root, _dirs, names in os.walk(src):
        for n in sorted(names):
            if n.endswith(".cpp"):
                files.append(os.path.join(root, n))
    f = _largest(files)
    if not f:
        return []
    text = open(f, errors="replace").read()
    body = "\n".join(l for l in text.splitlines() if not l.strip().startswith("//"))
    return _chunks(body, want, "\n")[:want]


def prose_chunks(want):
    # ONE document, consecutive sections -- same reasoning as code_chunks.
    docs = os.path.join(REPO, "docs")
    files = [os.path.join(docs, n) for n in sorted(os.listdir(docs))
             if n.endswith(".md")]
    f = _largest(files)
    if not f:
        return []
    text = open(f, errors="replace").read()
    # Strip fenced code and tables: this arm must be prose, or it is not a
    # control at all -- it is a mixture whose difference from the code arm
    # would be unattributable.
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = "\n".join(l for l in text.splitlines()
                     if not l.lstrip().startswith(("|", "#", "-", "*", "    ")))
    return _chunks(text, want, "\n\n")[:want]


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("code", "prose"):
        print(__doc__.strip().splitlines()[1], file=sys.stderr)
        return 2
    mode, out_path = sys.argv[1], sys.argv[2]
    want = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    chunks = code_chunks(want) if mode == "code" else prose_chunks(want)
    if len(chunks) < want:
        print("only %d chunks available for %s, wanted %d" % (len(chunks), mode, want),
              file=sys.stderr)
        return 1
    lens = [len(c) for c in chunks]
    json.dump({"responses": [{"content": c} for c in chunks]}, open(out_path, "w"))
    print("%s: %d chunks, chars min=%d max=%d mean=%d"
          % (mode, len(chunks), min(lens), max(lens), sum(lens) // len(lens)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
