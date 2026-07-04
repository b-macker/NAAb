"""Classify (VM result, tree-walker result, oracle expectation) triples.

Decision tree (see plan):
  1. crash/hang in either engine        -> CRASH / HANG          (sev 1)
  2. engines agree, oracle agrees       -> PASS
     engines agree, only libm lines off -> FLOAT_TOLERANCE       (sev 3)
     engines agree, oracle disagrees    -> SHARED_OR_ORACLE_GAP  (sev 2)
  3. engines disagree:
       allowlisted                      -> KNOWN
       one engine matches oracle        -> VM_BUG / TW_BUG       (sev 1)
       neither matches                  -> DOUBLE_MISMATCH       (sev 1)

Signatures are stable content hashes used for dedupe and for
tests/fuzzing/known_findings.txt entries.
"""

from __future__ import annotations

import hashlib
import math
import re
from dataclasses import dataclass
from typing import List, Optional

from .oracle import OracleResult, OutLine
from .runner import RunResult

ULP_TOLERANCE = 4

SEV = {
    "CRASH": 1, "HANG": 1, "VM_BUG": 1, "TW_BUG": 1, "DOUBLE_MISMATCH": 1,
    "SHARED_OR_ORACLE_GAP": 2,
    "FLOAT_TOLERANCE": 3,
    "KNOWN": 9, "PASS": 10,
    # engines agree, oracle couldn't model the program (OracleGap)
    "PASS_DIFF_ONLY": 10,
}


@dataclass
class Finding:
    classification: str
    signature: str
    detail: str
    seed: Optional[int] = None

    @property
    def severity(self) -> int:
        return SEV.get(self.classification, 2)


# --------------------------------------------------------- error categories

_CATEGORY_PATTERNS = [
    (re.compile(r"integer overflow", re.I), "integer_overflow"),
    (re.compile(r"division by zero", re.I), "division_by_zero"),
    (re.compile(r"modulo by zero", re.I), "modulo_by_zero"),
    (re.compile(r"type error", re.I), "type_error"),
    (re.compile(r"requires non-negative|domain error", re.I), "domain_error"),
    (re.compile(r"index|out of (range|bounds)", re.I), "index_error"),
]


def classify_error(error_text: str) -> Optional[str]:
    if not error_text:
        return None
    head = error_text.split("\n", 1)[0]
    for pat, tok in _CATEGORY_PATTERNS:
        if pat.search(head) or pat.search(error_text[:400]):
            return tok
    return "runtime_error"


# ------------------------------------------------------------- comparators


def _ulp_close(a: float, b: float, ulps: int = ULP_TOLERANCE) -> bool:
    if a == b:
        return True
    if math.isnan(a) and math.isnan(b):
        return True
    try:
        step = math.ulp(max(abs(a), abs(b)))
    except AttributeError:  # Python < 3.9
        step = max(abs(a), abs(b)) * 2 ** -52
    return abs(a - b) <= ulps * step


def lines_equal(got: List[str], want: List[OutLine]):
    """Compare engine lines vs oracle lines. Returns (ok, only_float_tol)."""
    if len(got) != len(want):
        return False, False
    float_tol_used = False
    for g, w in zip(got, want):
        if g == w.text:
            continue
        if w.approx:
            try:
                if _ulp_close(float(g), float(w.text)):
                    float_tol_used = True
                    continue
            except ValueError:
                pass
            return False, False
        return False, False
    return True, float_tol_used


def engines_agree(vm: RunResult, tw: RunResult) -> bool:
    if vm.rc != tw.rc:
        # Same failure category with different rc still counts as disagreement
        return False
    if vm.out_lines != tw.out_lines:
        return False
    if (vm.error_text == "") != (tw.error_text == ""):
        return False
    if vm.error_text and classify_error(vm.error_text) != classify_error(tw.error_text):
        return False
    return True


def _normalize_want(lines: List[OutLine]) -> List[OutLine]:
    # Trailing empty print lines are indistinguishable from the blank line
    # that precedes an error block; the runner strips them from engine
    # output, so strip them from the oracle side symmetrically.
    out = list(lines)
    while out and out[-1].text == "":
        out.pop()
    return out


def match_oracle(res: RunResult, oracle: OracleResult):
    """Returns (matches, only_float_tol)."""
    oracle = OracleResult(_normalize_want(oracle.lines), oracle.error)
    if oracle.error is not None:
        if res.rc == 0 or not res.error_text:
            return False, False
        if classify_error(res.error_text) != oracle.error:
            return False, False
        # Pre-error output must match the oracle's predicted prefix
        return lines_equal(res.out_lines, oracle.lines)
    if res.rc != 0 or res.error_text:
        return False, False
    return lines_equal(res.out_lines, oracle.lines)


# ----------------------------------------------------------------- triage


def _sig(*parts: str) -> str:
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:16]


def _crash_kind(r: RunResult) -> Optional[str]:
    if r.timed_out:
        return "timeout"
    if r.signal in (4, 6, 8, 11):  # ILL, ABRT, FPE, SEGV
        return "signal_%d" % r.signal
    return None


def triage(vm: RunResult, tw: RunResult, oracle: OracleResult,
           known_detectors=None) -> Finding:
    known_detectors = known_detectors or []

    # 1. crashes / hangs
    vk, tk = _crash_kind(vm), _crash_kind(tw)
    if vk and tk and vk == "timeout" and tk == "timeout":
        # both engines hung the same way: suspect generator/oracle, not engine
        return Finding("SHARED_OR_ORACLE_GAP", _sig("both_timeout"),
                       "both engines timed out — check loop bounds/oracle model")
    for engine, kind, res in (("vm", vk, vm), ("tw", tk, tw)):
        if kind:
            cls = "HANG" if kind == "timeout" else "CRASH"
            head = (res.error_text or res.stderr).split("\n")[0][:120]
            return Finding(cls, _sig(cls, engine, kind, head),
                           "%s %s in %s: %s" % (cls, kind, engine, head))

    # 2. engines agree?
    if engines_agree(vm, tw):
        ok, ftol = match_oracle(vm, oracle)
        if ok:
            return Finding("FLOAT_TOLERANCE" if ftol else "PASS",
                           "", "")
        return Finding(
            "SHARED_OR_ORACLE_GAP",
            _sig("shared", _diff_kind(vm, oracle)),
            "engines agree but oracle disagrees: %s" % _diff_detail(vm, oracle))

    # 3. engines disagree
    for det in known_detectors:
        if det(vm, tw):
            return Finding("KNOWN", _sig("known", det.__name__),
                           "allowlisted divergence: %s" % det.__name__)
    vm_ok, _ = match_oracle(vm, oracle)
    tw_ok, _ = match_oracle(tw, oracle)
    kind = _diff_kind_engines(vm, tw)
    if tw_ok and not vm_ok:
        return Finding("VM_BUG", _sig("vm_bug", kind),
                       "VM diverges from tree-walker+oracle: %s" % _engine_diff_detail(vm, tw))
    if vm_ok and not tw_ok:
        return Finding("TW_BUG", _sig("tw_bug", kind),
                       "tree-walker diverges from VM+oracle: %s" % _engine_diff_detail(vm, tw))
    return Finding("DOUBLE_MISMATCH", _sig("double", kind),
                   "engines disagree AND neither matches oracle: %s"
                   % _engine_diff_detail(vm, tw))


def _diff_kind(res: RunResult, oracle: OracleResult) -> str:
    if oracle.error is not None and not res.error_text:
        return "expected_error_got_none:%s" % oracle.error
    if oracle.error is None and res.error_text:
        return "unexpected_error:%s" % classify_error(res.error_text)
    if oracle.error is not None:
        return "error_category:%s_vs_%s" % (
            classify_error(res.error_text), oracle.error)
    if len(res.out_lines) != len(oracle.lines):
        return "line_count"
    for i, (g, w) in enumerate(zip(res.out_lines, oracle.lines)):
        if g != w.text and not w.approx:
            return "line_mismatch"
    return "float_mismatch"


def _diff_detail(res: RunResult, oracle: OracleResult) -> str:
    if oracle.error != (classify_error(res.error_text) if res.error_text else None):
        return "error %r vs oracle %r" % (
            classify_error(res.error_text) if res.error_text else None, oracle.error)
    for i, (g, w) in enumerate(zip(res.out_lines,
                                   [o.text for o in oracle.lines])):
        if g != w:
            return "line %d: engine=%r oracle=%r" % (i, g[:200], w[:200])
    return "line count %d vs %d" % (len(res.out_lines), len(oracle.lines))


def _diff_kind_engines(vm: RunResult, tw: RunResult) -> str:
    if vm.rc != tw.rc:
        return "rc:%s_vs_%s" % (vm.rc, tw.rc)
    if vm.error_text or tw.error_text:
        return "err:%s_vs_%s" % (classify_error(vm.error_text),
                                 classify_error(tw.error_text))
    for i, (a, b) in enumerate(zip(vm.out_lines, tw.out_lines)):
        if a != b:
            return "line"
    return "line_count"


def _engine_diff_detail(vm: RunResult, tw: RunResult) -> str:
    if vm.rc != tw.rc:
        return "rc vm=%d tw=%d; vm_err=%r tw_err=%r" % (
            vm.rc, tw.rc, vm.error_text[:120], tw.error_text[:120])
    for i, (a, b) in enumerate(zip(vm.out_lines, tw.out_lines)):
        if a != b:
            return "line %d: vm=%r tw=%r" % (i, a[:200], b[:200])
    return "line count vm=%d tw=%d" % (len(vm.out_lines), len(tw.out_lines))
