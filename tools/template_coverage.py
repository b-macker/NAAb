#!/usr/bin/env python3
"""Which govern.json settings does the loader read that govern-template.json
does not show?

govern-template.json (and its copy docs/govern-template.json) is the reference
users read to learn what govern.json can do. It drifted from the loader in both
directions: settings the loader reads were missing from it (output_admissibility,
deescalate_sustained, the S20-S23 signals, ...), and some it did show sat where
the loader never looks (subprocess_scrub_mode at the top level instead of under
capabilities.env_vars). Nothing checked either, so every sync was by hand.

This tool extracts every key path loadFromJson() in
src/runtime/governance_config.cpp reads and prints each LEAF the template does
not contain. tests/governance_v4/test_template_coverage.sh pins the result
against template_coverage_baseline.txt, the settings deliberately left out.

Run:  python3 tools/template_coverage.py [--template PATH | --template -] [--unread]

--unread also prints the REVERSE direction: template keys loadFromJson() never
reads. That list is SCREENED, not verified -- keys read outside loadFromJson
(update_reason, in reloadIfChanged) appear in it, and a key can be read by some
other loader (the scanner's own config). It is not gated; use it as a search
list, then confirm each entry by tracing before acting.

WHY NOT A GREP OF KEY NAMES. The loader reads through local aliases
(`auto& oa = cbj["output_admissibility"]; oa["threshold"]`), lambdas whose key is
a parameter (`loadSimpleCheck("no_dead_code", ...)` reading `cq[key]`), static
helpers (`parseRationale(obj, ...)` reads obj["rationale"]), loops over literal
key lists and `.items()` maps (agents.<name>.*). A leaf name says nothing about
the PATH it is read at, and the path is the whole question: the misplaced
subprocess keys have the right names. So this tokenises the loader and walks it
with a scope stack, binding each alias to the JSON path it denotes and each
lambda/helper parameter to its argument at the call site.

KNOWN LIMITS, stated so a reader does not over-trust it:
  * Static: a key read only in one branch of a type test (`if (v.is_object())`)
    is reported as read either way. The filters below remove the one systematic
    case (parseEnforcementLevel applied to a "level" string).
  * It sees only loadFromJson() and the helpers it calls. update_reason
    (reloadIfChanged) and extends/meta handling in loadFromFile are not in scope.
  * "Read" is not "does something". A key can be parsed and then consulted by
    nothing; that needs tracing to the point of effect, which this does not do.

EVERYTHING THIS PRINTS IS ASCII, written as bytes, for the reasons recorded in
tools/screen_state_fields.py (cp1252 stdout under MSYS2, CRLF from print()).

Exit 0 = the self-test passed. A non-zero exit means the INSTRUMENT is broken;
the list it printed is not a result.
"""
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOADER = os.path.join(REPO, "src", "runtime", "governance_config.cpp")
TEMPLATE = os.path.join(REPO, "govern-template.json")

# POSITIVE controls: each MUST come back as read, one per access shape the
# walker has to follow. If a refactor of the loader defeats one, the tool says
# so instead of reporting the key as missing from the template.
SELF_TEST_READ = [
    "circuit_breaker.output_admissibility.threshold",   # nested alias chain
    "code_quality.no_dead_code.patterns",               # lambda, key is a parameter
    "code_quality.no_hallucinated_apis.custom_patterns",  # lambda inside a block
    "capabilities.process.spawn",                       # loop over a literal key list
    "agents.<name>.api_base",                           # .items() structured binding
    "circuit_breaker.rationale",                        # static helper (parseRationale)
    "limits.data.dict_size",                            # capture-by-reference lambda
    "behavioral_sequences.patterns.[].decay_turns",     # array element + .value()
]
# NEGATIVE control: never read by the loader (its true home is
# capabilities.env_vars). Coming back as read means the walker over-attributes.
SELF_TEST_UNREAD = ["subprocess_scrub_mode"]
# String literals inside [] / .contains() that are NOT JSON keys. Any other
# unattributed literal is a read the walker failed to follow -- a blind spot,
# so it fails the self-test rather than silently under-reporting.
KNOWN_NON_JSON_LITERALS = {"telemetry.tamper_evidence.enabled"}  # explicitly_set.count(...)


# ---------------------------------------------------------------- tokenizer
TOK = re.compile(r'''
 (?P<ws>\s+)
|(?P<lc>//[^\n]*)
|(?P<bc>/\*.*?\*/)
|(?P<rstr>R"(?P<delim>[^(\s]*)\(.*?\)(?P=delim)")
|(?P<str>"(?:\\.|[^"\\\n])*")
|(?P<chr>'(?:\\.|[^'\\\n])*')
|(?P<num>\d[\w.']*)
|(?P<id>[A-Za-z_]\w*)
|(?P<op>::|->|&&|\|\||==|!=|<=|>=|\+\+|--|\+=|-=|[{}()\[\];,.<>=&*:?!+\-/%|^~#])
''', re.S | re.X)


def tokenize(text):
    toks, pos, line = [], 0, 1
    while pos < len(text):
        m = TOK.match(text, pos)
        if not m:
            raise RuntimeError("tokenizer stopped at line %d" % line)
        kind, val = m.lastgroup, m.group(0)
        if kind == "rstr":
            kind = "str"
        if kind not in ("ws", "lc", "bc"):
            toks.append((kind, val, line))
        line += val.count("\n")
        pos = m.end()
    return toks


class Extractor:
    """Walks loadFromJson() and records every JSON path it reads."""

    HELPERS = ("parseEnforcementLevel", "parseRationale", "warnEnableNeedsLevel",
               "warnIgnoredEnableFlag", "loadFromJson")
    DECL_TYPES = ("auto", "json", "string")

    def __init__(self, text):
        self.t = tokenize(text)
        self.n = len(self.t)
        self.reads = {}          # path tuple -> set of contexts
        self.resolved = set()    # token indexes of attributed string literals
        self.unresolved_keys = []
        self.funcs = {}
        for name in self.HELPERS:
            self.funcs[name] = self._find_function(name)

    # -- token helpers -------------------------------------------------
    def close(self, i):
        o = self.t[i][1]
        c = {"(": ")", "[": "]", "{": "}"}[o]
        d = 0
        for j in range(i, self.n):
            if self.t[j][0] != "op":
                continue
            if self.t[j][1] == o:
                d += 1
            elif self.t[j][1] == c:
                d -= 1
                if d == 0:
                    return j
        raise RuntimeError("unbalanced %s at line %d" % (o, self.t[i][2]))

    @staticmethod
    def lit(s):
        return bytes(s[1:-1], "utf-8").decode("unicode_escape")

    def split_args(self, a, b):
        out, d, s = [], 0, a
        for j in range(a, b):
            k, v, _ = self.t[j]
            if k != "op":
                continue
            if v in "([{":
                d += 1
            elif v in ")]}":
                d -= 1
            elif v == "," and d == 0:
                out.append((s, j))
                s = j + 1
        if s < b:
            out.append((s, b))
        return out

    def parse_params(self, a, b):
        params = []
        for s, e in self.split_args(a, b):
            words = [self.t[k][1] for k in range(s, e)]
            if not words:
                continue
            if "=" in words:
                words = words[:words.index("=")]
            ty = " ".join(words[:-1])
            kind = "json" if "json" in ty else ("str" if ("string" in ty or "char" in ty) else "other")
            params.append((kind, words[-1]))
        return params

    def _find_function(self, name):
        for i in range(1, self.n - 2):
            if (self.t[i][1] == name and self.t[i + 1][1] == "(" and
                    self.t[i - 1][1] in ("void", ">", "bool")):
                pc = self.close(i + 1)
                if self.t[pc + 1][1] == "{":
                    return {"params": self.parse_params(i + 2, pc), "body": (pc + 1, self.close(pc + 1))}
        raise RuntimeError("function not found in loader: " + name)

    def stmt_end(self, i):
        v = self.t[i][1]
        if v == "{":
            return self.close(i)
        if v in ("if", "while", "for", "switch"):
            e = self.stmt_end(self.close(i + 1) + 1)
            if v == "if" and e + 1 < self.n and self.t[e + 1][1] == "else":
                e = self.stmt_end(e + 2)
            return e
        if v == "else":
            return self.stmt_end(i + 1)
        d = 0
        for j in range(i, self.n):
            k, tv, _ = self.t[j]
            if k != "op":
                continue
            if tv in "([{":
                d += 1
            elif tv in ")]}":
                d -= 1
            elif tv == ";" and d == 0:
                return j
        raise RuntimeError("statement does not end")

    # -- environment -----------------------------------------------------
    @staticmethod
    def lookup(env, name):
        for scope in reversed(env):
            if name in scope:
                return scope[name]
        return None

    def record(self, path, ctx):
        self.reads.setdefault(path, set()).add(ctx)

    def key_of(self, i, env):
        kind, val, _ = self.t[i]
        if kind == "str":
            return self.lit(val), True
        if kind == "num":
            return "[]", True
        if kind == "id":
            b = self.lookup(env, val)
            if b and b[0] == "str":
                return b[1], True
            if b and b[0] == "mapkey":
                return "<" + val + ">", True
            return "<?" + val + ">", False
        return "<?expr>", False

    def chain(self, i, env, ctx, rec=True):
        """t[i] is an identifier bound to a JSON path; follow its postfix chain."""
        path = self.lookup(env, self.t[i][1])[1]
        j = i + 1
        while j < self.n:
            v = self.t[j][1]
            if v == "[":
                c = self.close(j)
                if c != j + 2:
                    return path, j
                key, ok = self.key_of(j + 1, env)
                if not ok:
                    self.unresolved_keys.append((self.t[j][2], ".".join(path)))
                path = path + (key,)
                if rec:
                    self.record(path, ctx)
                    if self.t[j + 1][0] == "str":
                        self.resolved.add(j + 1)
                j = c + 1
                continue
            if v == "." and j + 2 < self.n and self.t[j + 2][1] == "(":
                meth = self.t[j + 1][1]
                c = self.close(j + 2)
                if meth in ("contains", "count", "find", "value", "at"):
                    args = self.split_args(j + 3, c)
                    if args:
                        a0 = args[0][0]
                        key, ok = self.key_of(a0, env)
                        if not ok:
                            self.unresolved_keys.append((self.t[j][2], ".".join(path)))
                        if rec:
                            self.record(path + (key,), ctx)
                            if self.t[a0][0] == "str":
                                self.resolved.add(a0)
                        if meth == "at":
                            path = path + (key,)
                            j = c + 1
                            continue
                    return None, j
                return path, j
            return path, j
        return path, j

    def json_expr(self, a, b, env):
        while a < b and self.t[a][1] in ("&", "*"):
            a += 1
        if a < b and self.t[a][0] == "id":
            bnd = self.lookup(env, self.t[a][1])
            if bnd and bnd[0] == "json":
                p, j = self.chain(a, env, "", rec=False)
                if p is not None and j == b:
                    return p
        return None

    def str_expr(self, a, b, env):
        ts = self.t[a:b]
        if len(ts) == 1 and ts[0][0] == "str":
            return self.lit(ts[0][1])
        if any(x[1] == "?" for x in ts):   # cond ? "a" : "b"  -> first branch
            q = [k for k, x in enumerate(ts) if x[1] == "?"][0]
            for x in ts[q:]:
                if x[0] == "str":
                    return self.lit(x[1])
        if len(ts) == 1 and ts[0][0] == "id":
            bnd = self.lookup(env, ts[0][1])
            if bnd and bnd[0] == "str":
                return bnd[1]
        return None

    # -- the walk ---------------------------------------------------------
    def walk(self, a, b, env, ctx, depth=0):
        if depth > 40:
            raise RuntimeError("walk recursion limit")
        i = a
        while i < b:
            kind, v, _ = self.t[i]
            if kind == "op" and v == "{":
                c = self.close(i)
                self.walk(i + 1, c, env + [{}], ctx, depth)
                i = c + 1
                continue
            if v == "for" and self.t[i + 1][1] == "(":
                done = self._range_for(i, env, ctx, depth)
                if done is not None:
                    i = done
                    continue
            if kind == "id" and v in self.DECL_TYPES:
                done = self._declaration(i, b, env, ctx, depth)
                if done is not None:
                    i = done
                    continue
            if kind == "id" and self.t[i + 1][1] == "(":
                bnd = self.lookup(env, v)
                fn = bnd[1] if (bnd and bnd[0] == "lambda") else self.funcs.get(v)
                if fn is not None and v != "loadFromJson":
                    i = self._call(i, fn, env, ctx + ">" + v, depth)
                    continue
            if kind == "id":
                bnd = self.lookup(env, v)
                if bnd and bnd[0] == "json":
                    _, j = self.chain(i, env, ctx)
                    i = max(j, i + 1)
                    continue
            i += 1

    def _range_for(self, i, env, ctx, depth):
        c = self.close(i + 1)
        colon, d = None, 0
        for j in range(i + 2, c):
            tv = self.t[j][1]
            if tv in "([{":
                d += 1
            elif tv in ")]}":
                d -= 1
            elif tv == ":" and d == 0:
                colon = j
                break
        if colon is None:
            return None
        lhs = [self.t[j][1] for j in range(i + 2, colon)]
        body_s = c + 1
        body_e = self.stmt_end(body_s)
        ra, rb = colon + 1, c
        self.walk(ra, rb, env, ctx, depth + 1)
        if "[" in lhs:                       # for (auto& [k, v] : X.items())
            names = [x for x in lhs[lhs.index("[") + 1: lhs.index("]")] if x != ","]
            scope = {}
            if self.t[rb - 1][1] == ")" and self.t[rb - 2][1] == "(" and self.t[rb - 3][1] == "items":
                p = self.json_expr(ra, rb - 4, env)
                if p is not None:
                    entry = p + ("<" + names[0] + ">",)
                    scope[names[0]] = ("mapkey", p)
                    scope[names[1]] = ("json", entry)
                    self.record(entry, ctx)  # iterating a map consumes its entries
            self.walk(body_s, body_e + 1, env + [scope], ctx, depth + 1)
            return body_e + 1
        name = lhs[-1]
        if self.t[ra][1] == "{":             # for (const char* k : {"a", "b"})
            for j in range(ra, rb):
                if self.t[j][0] == "str":
                    self.walk(body_s, body_e + 1, env + [{name: ("str", self.lit(self.t[j][1]))}],
                              ctx, depth + 1)
            return body_e + 1
        p = self.json_expr(ra, rb, env)
        scope = {name: ("json", p + ("[]",))} if p is not None else {}
        self.walk(body_s, body_e + 1, env + [scope], ctx, depth + 1)
        return body_e + 1

    def _declaration(self, i, b, env, ctx, depth):
        j = i + 1
        while j < b and self.t[j][1] in ("&", "&&", "*", "const"):
            j += 1
        if not (j + 2 < b and self.t[j][0] == "id" and self.t[j + 1][1] == "=" and self.t[j + 2][1] != "="):
            return None
        name, rhs = self.t[j][1], j + 2
        if self.t[rhs][1] == "[":            # lambda: bind, instantiate at call sites
            cap_e = self.close(rhs)
            if self.t[cap_e + 1][1] == "(":
                pe = self.close(cap_e + 1)
                bs = pe + 1
                while self.t[bs][1] != "{":
                    bs += 1
                be = self.close(bs)
                env[-1][name] = ("lambda", {"params": self.parse_params(cap_e + 2, pe), "body": (bs, be)})
                return be + 1
        se = self.stmt_end(rhs)
        p = self.json_expr(rhs, se, env)
        if p is not None:
            env[-1][name] = ("json", p)
        else:
            s = self.str_expr(rhs, se, env)
            if s is not None:
                env[-1][name] = ("str", s)
        self.walk(rhs, se, env, ctx, depth + 1)
        return se + 1

    def _call(self, i, fn, env, ctx, depth):
        c = self.close(i + 1)
        scope = {}
        for (pk, pn), (s, e) in zip(fn["params"], self.split_args(i + 2, c)):
            if pk == "json":
                p = self.json_expr(s, e, env)
                if p is not None:
                    scope[pn] = ("json", p)
            elif pk == "str":
                sv = self.str_expr(s, e, env)
                if sv is not None:
                    scope[pn] = ("str", sv)
        self.walk(i + 2, c, env, ctx, depth + 1)
        self.walk(fn["body"][0] + 1, fn["body"][1], env + [scope], ctx, depth + 1)
        return c + 1

    def run(self):
        a, b = self.funcs["loadFromJson"]["body"]
        self.walk(a + 1, b, [{"j": ("json", ())}], "loadFromJson")
        unattributed = []
        for i in range(a, b):
            kind, val, _ = self.t[i]
            if kind != "str" or i in self.resolved:
                continue
            prev, prev2 = self.t[i - 1][1], self.t[i - 2][1]
            if ((prev == "[" and self.t[i + 1][1] == "]") or
                    (prev == "(" and prev2 in ("contains", "value", "count", "find", "at"))):
                unattributed.append(self.lit(val))
        return unattributed


# ---------------------------------------------------------------- comparison
LEVEL_VALUE_KEYS = ("level", "missing_level", "cross_block_level")


def is_placeholder(seg):
    return seg.startswith("<")


def missing_leaves(reads, template):
    """Read leaves with no node in the template. Filters, each a systematic
    artifact rather than a setting:
      * parseEnforcementLevel(x["level"]) statically 'reads' level.enabled and
        level.level, which apply only if a level were an object;
      * per-block "rationale", when the template documents it once in a
        top-level _comment_rationale;
      * a dynamic map's entry placeholder, when the map itself is present."""
    trie = {}
    for path in reads:
        node = trie
        for seg in path:
            node = node.setdefault(seg, {})

    covered = set()

    def walk(val, node, path):
        covered.add(tuple(path))
        if isinstance(val, dict):
            for k, v in val.items():
                if k.startswith("_comment"):
                    continue
                if k in node:
                    walk(v, node[k], path + [k])
                else:
                    ph = [c for c in node if is_placeholder(c)]
                    if ph:
                        walk(v, node[ph[0]], path + [ph[0]])
        elif isinstance(val, list) and "[]" in node:
            for el in val:
                walk(el, node["[]"], path + ["[]"])

    walk(template, trie, [])
    rationale_documented = isinstance(template, dict) and "_comment_rationale" in template

    out = []

    def leaves(node, path):
        if not node:
            p = tuple(path)
            if p in covered:
                return
            ctxs = reads.get(p, set())
            if (len(p) >= 2 and p[-2] in LEVEL_VALUE_KEYS and p[-1] in ("enabled", "level")
                    and any("parseEnforcementLevel" in c for c in ctxs)):
                return
            if rationale_documented and p[-1] == "rationale":
                return
            if is_placeholder(p[-1]) and p[:-1] in covered:
                return
            out.append(".".join(p))
            return
        for k, v in node.items():
            leaves(v, path + [k])

    leaves(trie, [])
    return sorted(out)


def unread_template_keys(reads, template):
    """Template leaves (and whole sections) with no read path. SCREENED."""
    trie = {}
    for path in reads:
        node = trie
        for seg in path:
            node = node.setdefault(seg, {})
    out = []

    def walk(val, node, path):
        if isinstance(val, dict):
            for k, v in val.items():
                if k.startswith("_comment"):
                    continue
                if k in node:
                    walk(v, node[k], path + [k])
                else:
                    ph = [c for c in node if is_placeholder(c)]
                    if ph:
                        walk(v, node[ph[0]], path + [k])
                    else:
                        out.append(".".join(path + [k]))
        elif isinstance(val, list) and "[]" in node:
            for el in val:
                walk(el, node["[]"], path + ["[]"])

    walk(template, trie, [])
    return sorted(set(out))


def emit(lines):
    sys.stdout.buffer.write(("\n".join(lines) + "\n").encode("ascii", "replace"))


def main(argv):
    template_path = TEMPLATE
    want_unread = "--unread" in argv
    argv = [a for a in argv if a != "--unread"]
    if len(argv) >= 2 and argv[0] == "--template":
        template_path = argv[1]
    if template_path == "-":
        template = json.loads(sys.stdin.buffer.read().decode("utf-8"))
    else:
        with open(template_path, "rb") as f:
            template = json.loads(f.read().decode("utf-8"))
    with open(LOADER, "rb") as f:
        ex = Extractor(f.read().decode("utf-8", errors="replace"))
    unattributed = ex.run()

    failures = []
    read_names = {".".join(p) for p in ex.reads}
    for want in SELF_TEST_READ:
        if want not in read_names:
            failures.append("!! SELF-TEST FAIL: known-read path not extracted: " + want)
    for never in SELF_TEST_UNREAD:
        if never in read_names:
            failures.append("!! SELF-TEST FAIL: path the loader never reads came back read: " + never)
    for s in unattributed:
        if s not in KNOWN_NON_JSON_LITERALS:
            failures.append("!! SELF-TEST FAIL: string-literal key the walker could not attribute "
                            "to a path (a new access shape?): " + s)
    for line, base in ex.unresolved_keys:
        failures.append("!! SELF-TEST FAIL: variable key not bound at loader line %d under %s" % (line, base))
    if len(ex.reads) < 500:
        failures.append("!! SELF-TEST FAIL: only %d paths extracted -- the walk did not run "
                        "against the real loader" % len(ex.reads))

    lines = ["loader-paths: %d" % len(ex.reads)]
    lines += failures
    lines.append("self-test failures: %d" % len(failures))
    lines += ["missing: " + p for p in missing_leaves(ex.reads, template)]
    if want_unread:
        lines += ["unread (screened): " + p for p in unread_template_keys(ex.reads, template)]
    emit(lines)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
