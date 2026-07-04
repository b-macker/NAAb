"""Run NAAb programs on both engines and capture normalized results.

Empirical facts this module encodes (verified against build/naab-lang):
  - Runtime errors are printed to STDOUT, not stderr. Program output and the
    error block share the stream; we split at the first error-header line.
  - Output can contain ANSI color codes; strip them before comparing.
  - Programs run from a clean temp cwd with --no-governance --timeout N so no
    repo govern.json is picked up (sandbox fail-closed gotcha).
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
from dataclasses import dataclass, field
from typing import List, Optional

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# First line of a runtime error block on stdout.
ERROR_HEAD_RE = re.compile(
    r"^(?:\[?(?:Runtime|Math|Type|Index|Value|Argument|Range) error\b"
    r"|Error:|error:"
    r"|Division by zero|Modulo by zero"
    r"|.*\berror\b.*:)",
    re.IGNORECASE,
)

INTERNAL_TIMEOUT = 15   # --timeout passed to naab-lang
SUBPROCESS_TIMEOUT = 25  # hard kill from the harness side


@dataclass
class RunResult:
    rc: int
    out_lines: List[str] = field(default_factory=list)  # pre-error stdout
    error_text: str = ""     # error block (stdout tail), "" if none
    stderr: str = ""
    timed_out: bool = False
    signal: Optional[int] = None  # populated for signal deaths


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def split_error_block(stdout: str):
    """Split stdout into (program lines, error block text)."""
    lines = strip_ansi(stdout).split("\n")
    for i, ln in enumerate(lines):
        if ERROR_HEAD_RE.match(ln.strip()):
            head = [l.rstrip() for l in lines[:i]]
            while head and head[-1] == "":
                head.pop()
            return head, "\n".join(lines[i:]).strip()
    head = [l.rstrip() for l in lines]
    while head and head[-1] == "":
        head.pop()
    return head, ""


def run_engine(naab_bin: str, source: str, tree_walk: bool,
               workdir: Optional[str] = None) -> RunResult:
    own_dir = None
    if workdir is None:
        own_dir = tempfile.TemporaryDirectory(prefix="naabfuzz_")
        workdir = own_dir.name
    try:
        src_path = os.path.join(workdir, "prog.naab")
        with open(src_path, "w") as f:
            f.write(source)
        cmd = [os.path.abspath(naab_bin), "--no-governance",
               "--timeout", str(INTERNAL_TIMEOUT)]
        if tree_walk:
            cmd.append("--tree-walk")
        cmd.append("prog.naab")
        try:
            p = subprocess.run(
                cmd, cwd=workdir, capture_output=True, text=True,
                timeout=SUBPROCESS_TIMEOUT, errors="replace",
                env={**os.environ, "NO_COLOR": "1", "TERM": "dumb"},
            )
        except subprocess.TimeoutExpired as te:
            partial = te.stdout or ""
            if isinstance(partial, bytes):
                partial = partial.decode("utf-8", "replace")
            out, err_block = split_error_block(partial)
            return RunResult(rc=124, out_lines=out, error_text=err_block,
                             timed_out=True)
        out, err_block = split_error_block(p.stdout)
        sig = -p.returncode if p.returncode < 0 else (
            p.returncode - 128 if p.returncode > 128 else None)
        return RunResult(rc=p.returncode, out_lines=out, error_text=err_block,
                         stderr=strip_ansi(p.stderr), signal=sig)
    finally:
        if own_dir is not None:
            own_dir.cleanup()


def run_both(naab_bin: str, source: str):
    """Returns (vm: RunResult, tw: RunResult)."""
    return (run_engine(naab_bin, source, tree_walk=False),
            run_engine(naab_bin, source, tree_walk=True))
