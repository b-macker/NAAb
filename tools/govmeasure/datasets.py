#!/usr/bin/env python3
"""Fetch the measurement's datasets, pinned by SHA-256 (Bundle 13 PR 2).

    datasets.py fetch DIR [NAME...]     download into DIR (all, or the named ones)
    datasets.py verify DIR [NAME...]    check what is already there
    datasets.py list

An entry with "git" and "commit" instead of "url" is a repository: that one
commit is fetched (depth 1) into DIR/NAME/ and kept only when the checkout's
HEAD is exactly the pinned commit.

Nothing is committed: a dataset is downloaded at run time from the URL in
datasets.json and kept only when its hash matches the pin; a mismatch deletes
the file and fails (exit 1), so a changed upstream file is never measured as
if it were the pinned one. A dataset that cannot be downloaded exits 2
(UNMEASURABLE: the network, not the data). curl does the transfer, so the
environment's proxy and CA settings apply.
"""

import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def out(text):
    sys.stdout.buffer.write((text + "\n").encode("ascii", "replace"))


def load():
    with open(os.path.join(HERE, "datasets.json"), "r", encoding="utf-8") as f:
        doc = json.load(f)
    return {k: v for k, v in doc.items() if not k.startswith("_")}


def filename(name, entry):
    if "git" in entry:
        return name
    return name + os.path.splitext(entry["url"])[1].replace(".gz", ".jsonl.gz")


def git_head(directory):
    try:
        p = subprocess.run(["git", "-C", directory, "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
    except OSError:
        return ""
    return p.stdout.decode("ascii", "replace").strip() if p.returncode == 0 else ""


def fetch_git(name, entry, dest):
    """"pass", "fail" (a checkout that is not the pinned commit: removed) or
    "unmeasurable" (it could not be fetched)."""
    import shutil
    if git_head(dest) != entry["commit"]:
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        steps = [["git", "-C", dest, "init", "-q"],
                 ["git", "-C", dest, "remote", "add", "origin", entry["git"]],
                 ["git", "-C", dest, "fetch", "-q", "--depth", "1", "origin", entry["commit"]],
                 ["git", "-C", dest, "checkout", "-q", "FETCH_HEAD"]]
        for step in steps:
            try:
                rc = subprocess.call(step, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
            except (OSError, subprocess.TimeoutExpired):
                rc = 1
            if rc != 0:
                shutil.rmtree(dest, ignore_errors=True)
                return "unmeasurable"
    if git_head(dest) != entry["commit"]:
        shutil.rmtree(dest, ignore_errors=True)
        return "fail"
    return "pass"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def path_of(directory, name):
    return os.path.join(directory, filename(name, load()[name]))


def main(argv):
    if not argv or argv[0] not in ("fetch", "verify", "list"):
        sys.stderr.write(__doc__)
        return 4
    sets = load()
    if argv[0] == "list":
        for name in sorted(sets):
            out("%s  %s" % (name, sets[name]["what"]))
        return 0
    if len(argv) < 2:
        sys.stderr.write("datasets.py %s DIR [NAME...]\n" % argv[0])
        return 4
    directory = argv[1]
    names = argv[2:] or sorted(sets)
    unknown = [n for n in names if n not in sets]
    if unknown:
        sys.stderr.write("datasets.py: unknown dataset(s): %s\n" % ", ".join(unknown))
        return 4
    os.makedirs(directory, exist_ok=True)
    worst = 0
    failed = False
    for name in names:
        entry = sets[name]
        dest = os.path.join(directory, filename(name, entry))
        if "git" in entry:
            if argv[0] == "fetch":
                got = fetch_git(name, entry, dest)
            else:
                got = "pass" if git_head(dest) == entry["commit"] else "unmeasurable"
            if got == "pass":
                out("PASS %s: %s" % (name, dest))
            elif got == "fail":
                out("FAIL %s: the checkout is not the pinned commit %s -- removed" % (name, entry["commit"]))
                failed = True
            else:
                out("UNMEASURABLE %s: could not fetch commit %s" % (name, entry["commit"]))
                worst = max(worst, 2)
            continue
        if argv[0] == "fetch" and not (os.path.exists(dest) and sha256(dest) == entry["sha256"]):
            rc = subprocess.call(["curl", "-sS", "-L", "--fail", "--max-time", "1800",
                                  "-o", dest + ".part", entry["url"]])
            if rc != 0:
                out("UNMEASURABLE %s: download failed (curl exit %d)" % (name, rc))
                try:
                    os.remove(dest + ".part")
                except OSError:
                    pass
                worst = max(worst, 2)
                continue
            os.replace(dest + ".part", dest)
        if not os.path.exists(dest):
            out("UNMEASURABLE %s: not present" % name)
            worst = max(worst, 2)
            continue
        got = sha256(dest)
        if got != entry["sha256"]:
            out("FAIL %s: sha256 %s does not match the pin %s -- removed" % (name, got, entry["sha256"]))
            os.remove(dest)
            failed = True
            continue
        out("PASS %s: %s" % (name, dest))
    return 1 if failed else worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
