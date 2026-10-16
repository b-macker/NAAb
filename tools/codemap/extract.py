"""codemap extractor: one translation unit -> one JSON shard.

Reads a compile command from build/compile_commands.json, parses the file with
libclang and records what a reviewer otherwise finds by grep sweeps:

  symbols   functions, methods, constructors, lambdas (pseudo-symbols), classes
  call      caller -> callee at a call site
  ref       a function used as a VALUE (a callback, a std::function binding);
            it may be called indirectly from somewhere the AST cannot see
  defines   function -> lambda it defines (who invokes the lambda is dynamic)
  override  method -> the base method it overrides
  thread    function -> the lambda/function it starts on a thread
            (std::thread, std::jthread, std::async, a pool's enqueue/submit)
  dispatch  NAAb name -> C++ code: a stdlib `module.fn` branch inside a
            module's call(), and `registerExecutor("lang", make_unique<X>)`
  include   file -> file it includes (repo files only)
  write     function -> a field or non-local variable it assigns (=, op=,
            ++/--, including overloaded operators on std::atomic/std::string)
  mutate    function -> a field it changes through a mutating method
            (store, fetch_add, push_back, insert, erase, clear, ...)
  predispatch  NAAb name -> the gates the VM and tree-walker run BEFORE
            dispatching it, read from GovernanceEngine::filesystemAccessMode
            (applied at merge only if every caller of that allowlist calls the
            gates; see store.py)

A lambda stored in a variable (`auto callToolLoopTurn = [&](...) {...}`) is
named after the variable, so it can be found by that name.

Calls inside lambdas are kept and attributed to the lambda, which hangs off the
function that defines it. clangd 18's call hierarchy dropped such sites (4 of
15 callers of ErrorSanitizer::sanitize, all inside REST route lambdas, while its
own references query found all 15) -- that loss is why this is not a clangd
client.

Only code under the repository is walked; system headers and external/ are
skipped. Code excluded by the preprocessor for this build (#ifdef _WIN32 on
Linux) is not seen at all.
"""

import ctypes
import hashlib
import json
import os
import re
import sys
import time

import clang.cindex as ci

K = ci.CursorKind
FUNC_KINDS = {K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.DESTRUCTOR,
              K.FUNCTION_TEMPLATE, K.CONVERSION_FUNCTION}
CLASS_KINDS = {K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE,
               K.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION}


def canonical_params(ch):
    """`(const std::basic_string<char> &, int) const`: the parameter types as the
    compiler sees them (aliases resolved), so a profile frame -- which a demangler
    prints canonically -- can be matched to ONE overload. displayname keeps the
    types as written (`const std::string &`) and cannot be compared."""
    try:
        if ch.kind == K.FUNCTION_TEMPLATE:
            # get_arguments() is empty for a template; its parameters are children
            params = [c.type.get_canonical().spelling for c in ch.get_children() if c.kind == K.PARM_DECL]
        else:
            params = [a.type.get_canonical().spelling for a in ch.get_arguments()]
        if ch.type.kind == ci.TypeKind.FUNCTIONPROTO and ch.type.is_function_variadic():
            params.append("...")
        const = ch.kind == K.CXX_METHOD and ch.is_const_method()
    except Exception:   # a cursor the bindings cannot describe: no signature, never a guess
        return ""
    return "(%s)%s" % (", ".join(params), " const" if const else "")
# Expressions a callee reference can sit inside without stopping being the
# callee: `f(x)` is CALL_EXPR > UNEXPOSED_EXPR > DECL_REF_EXPR(f).
WRAPPERS = {K.UNEXPOSED_EXPR, K.PAREN_EXPR}
THREAD_CALLEES = re.compile(r"^std::(thread|jthread)::(thread|jthread)$|^std::async$|^pthread_create$")
THREAD_TYPES = re.compile(r"^(const )?std::(thread|jthread)$")
POOL_METHODS = {"enqueue", "submit", "post"}

_overridden_ready = False


def _setup_overridden():
    """clang_getOverriddenCursors is not wrapped by the Python bindings."""
    global _overridden_ready
    lib = ci.conf.lib
    lib.clang_getOverriddenCursors.argtypes = [
        ci.Cursor, ctypes.POINTER(ctypes.POINTER(ci.Cursor)), ctypes.POINTER(ctypes.c_uint)]
    lib.clang_getOverriddenCursors.restype = None
    lib.clang_disposeOverriddenCursors.argtypes = [ctypes.POINTER(ci.Cursor)]
    lib.clang_disposeOverriddenCursors.restype = None
    _overridden_ready = True


_opkinds_ready = False


def _setup_opkinds():
    global _opkinds_ready
    lib = ci.conf.lib
    lib.clang_getCursorBinaryOperatorKind.argtypes = [ci.Cursor]
    lib.clang_getCursorBinaryOperatorKind.restype = ctypes.c_int
    lib.clang_getCursorUnaryOperatorKind.argtypes = [ci.Cursor]
    lib.clang_getCursorUnaryOperatorKind.restype = ctypes.c_int
    for fn in ("clang_getBinaryOperatorKindSpelling", "clang_getUnaryOperatorKindSpelling"):
        getattr(lib, fn).argtypes = [ctypes.c_int]
        getattr(lib, fn).restype = ci._CXString
        getattr(lib, fn).errcheck = ci._CXString.from_result
    _opkinds_ready = True


_op_cache = {}


def operator_spelling(c):
    """'=', '+=', '++', ... for a BINARY/COMPOUND_ASSIGNMENT/UNARY_OPERATOR cursor."""
    if not _opkinds_ready:
        _setup_opkinds()
    lib = ci.conf.lib
    if c.kind == K.UNARY_OPERATOR:
        kind = lib.clang_getCursorUnaryOperatorKind(c)
        key = ("u", kind)
        if key not in _op_cache:
            _op_cache[key] = lib.clang_getUnaryOperatorKindSpelling(kind)
    else:
        kind = lib.clang_getCursorBinaryOperatorKind(c)
        key = ("b", kind)
        if key not in _op_cache:
            _op_cache[key] = lib.clang_getBinaryOperatorKindSpelling(kind)
    return _op_cache[key]


ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
INCDEC_OPS = {"++", "--"}
ASSIGN_OPERATOR_FNS = {"operator" + o for o in ASSIGN_OPS | INCDEC_OPS}
# Methods that change the object they are called on. Screened: a name list,
# so a mutator not named here is a blind spot (stated in BLIND_SPOTS).
MUTATING_METHODS = {"store", "fetch_add", "fetch_sub", "fetch_and", "fetch_or", "fetch_xor", "exchange",
                    "compare_exchange_weak", "compare_exchange_strong", "push_back", "emplace_back",
                    "push_front", "emplace_front", "pop_back", "pop_front", "insert", "emplace",
                    "insert_or_assign", "try_emplace", "erase", "clear", "reset", "swap", "assign",
                    "append", "resize", "push", "pop", "merge", "extract"}
# The pre-dispatch allowlist: a function `(module, method) -> mode` the VM and
# tree-walker consult before dispatching a NAAb stdlib call.
PREDISPATCH_ALLOWLIST = "filesystemAccessMode"
VAR_KEEP = {K.UNEXPOSED_EXPR, K.PAREN_EXPR, K.TYPE_REF, K.NAMESPACE_REF, K.TEMPLATE_REF}


def _overridden(c):
    if not _overridden_ready:
        _setup_overridden()
    arr = ctypes.POINTER(ci.Cursor)()
    n = ctypes.c_uint(0)
    ci.conf.lib.clang_getOverriddenCursors(c, ctypes.byref(arr), ctypes.byref(n))
    out = [arr[i].get_usr() for i in range(n.value)]
    if n.value:
        ci.conf.lib.clang_disposeOverriddenCursors(arr)
    return out


def qname(c):
    parts = []
    while c is not None and c.kind != K.TRANSLATION_UNIT:
        if c.spelling:
            parts.append(c.spelling)
        c = c.semantic_parent
    return "::".join(reversed(parts))


def compile_args(cmd, source):
    """Arguments for libclang: the compile command minus compiler, -o/-c and the source."""
    args, skip = [], False
    for a in list(cmd.arguments)[1:]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c":
            continue
        if os.path.abspath(os.path.join(cmd.directory, a)) == source:
            continue
        args.append(a)
    return args


def args_hash(args):
    return hashlib.sha256("\0".join(args).encode()).hexdigest()[:16]


class Extractor:
    def __init__(self, repo, tu, source):
        self.repo = repo
        self.tu = tu
        self.source = source
        self.symbols = {}
        self.classes = {}
        self.edges = set()
        self.dispatch_ranges = {}
        self.modules = {}       # class usr -> module name from getName()
        self.lambdas = {}       # (file, line, col) of a lambda -> its pseudo-symbol
        self.predispatch = None  # the parsed pre-dispatch allowlist, if this TU defines it
        self._in_repo = {}

    # -- helpers ---------------------------------------------------------
    def rel(self, path):
        return os.path.relpath(path, self.repo)

    def in_repo(self, c):
        f = c.location.file
        if f is None:
            return None
        name = f.name
        hit = self._in_repo.get(name)
        if hit is None:
            ap = os.path.abspath(name)
            hit = ap.startswith(self.repo + os.sep) and (os.sep + "external" + os.sep) not in ap \
                and (os.sep + "build" + os.sep) not in ap[len(self.repo):]
            self._in_repo[name] = hit
        return hit

    def site(self, c):
        f = c.location.file
        if f is None:          # implicit nodes carry no location
            return None, 0
        return self.rel(f.name), c.location.line

    def add_symbol(self, usr, name, kind, c, extra=None):
        file, line = self.site(c)
        rec = {"usr": usr, "name": name, "kind": kind, "file": file, "line": line,
               "end_line": c.extent.end.line, "is_def": 1}
        if extra:
            rec.update(extra)
        self.symbols[usr] = rec

    def edge(self, kind, src, dst, c, note=""):
        file, line = self.site(c)
        if file is None or not src or not dst:
            return
        self.edges.add((kind, src, dst, file, line, note))

    def lambda_of(self, t):
        """A call to a lambda's operator() -> the lambda's pseudo-symbol, if seen."""
        if t.kind != K.CXX_METHOD or t.spelling != "operator()":
            return None
        parent = t.semantic_parent
        if parent is None or not (parent.spelling or "").startswith("(lambda at"):
            return None
        file, line = self.site(parent)
        return self.lambdas.get((file, line, parent.location.column))

    def note_target(self, t):
        """Make sure a callee/ref target has at least a declaration record."""
        usr = t.get_usr()
        if usr and usr not in self.symbols and t.location.file is not None:
            file = self.rel(t.location.file.name) if self.in_repo(t) else t.location.file.name
            self.symbols.setdefault(usr, {"usr": usr, "name": qname(t), "kind": t.kind.name.lower(),
                                          "file": file, "line": t.location.line,
                                          "end_line": t.location.line, "is_def": 0})
        return usr

    # -- walk --------------------------------------------------------------
    def run(self):
        root_ctx = {"fn": None, "callee": None, "thread": None, "dispatch": None,
                    "in_call_method": None, "var": None}
        # Explicit stack: deep expression trees exceed Python's recursion limit.
        # Each entry is a node still to be visited; visit() returns its children.
        stack = [(ch, root_ctx) for ch in reversed(list(self.tu.cursor.get_children()))]
        while stack:
            node, ctx = stack.pop()
            if self.in_repo(node) is False:
                continue
            stack.extend(reversed(self.visit(node, ctx)))
        return self.result()

    def enclosing(self, ctx, c):
        if ctx["fn"]:
            return ctx["fn"]
        # file-scope code (static initializers) gets one pseudo-symbol per file
        file, _ = self.site(c)
        if file is None:
            return None
        usr = "codemap:static-init:" + file
        self.symbols.setdefault(usr, {"usr": usr, "name": "<static-init " + file + ">", "kind": "static_init",
                                      "file": file, "line": 1, "end_line": 1, "is_def": 1})
        return usr

    def visit(self, ch, ctx):
        """Yield (child, ctx) pairs to walk next."""
        k = ch.kind
        new = dict(ctx)

        if k in FUNC_KINDS and ch.is_definition():
            usr = ch.get_usr()
            extra = {"sig": ch.displayname, "psig": canonical_params(ch)}
            parent = ch.semantic_parent
            if parent is not None and parent.kind in CLASS_KINDS:
                extra["class_usr"] = parent.get_usr()
            if k == K.CXX_METHOD and ch.is_virtual_method():
                extra["virtual"] = 2 if ch.is_pure_virtual_method() else 1
                for base in _overridden(ch):
                    self.edges.add(("override", usr, base, self.site(ch)[0], ch.location.line, ""))
            self.add_symbol(usr, qname(ch), k.name.lower(), ch, extra)
            new.update(fn=usr, callee=None, thread=None, dispatch=None,
                       in_call_method=(parent.get_usr() if k == K.CXX_METHOD and ch.spelling == "call"
                                       and parent is not None else None))
            if k == K.CXX_METHOD and ch.spelling == "getName":
                lit = self.first_string(ch)
                if lit is not None and parent is not None:
                    self.modules[parent.get_usr()] = lit
            if ch.spelling == PREDISPATCH_ALLOWLIST:
                self.predispatch = self.read_allowlist(ch, usr)
            new["var"] = None
            return [(x, new) for x in ch.get_children()]

        if k in FUNC_KINDS:
            # A declaration: record it (a pure virtual has nothing else) and the
            # virtual relationship. A definition seen anywhere replaces it at merge.
            usr = ch.get_usr()
            file, line = self.site(ch)
            if usr and file is not None and usr not in self.symbols:
                rec = {"usr": usr, "name": qname(ch), "kind": k.name.lower(), "file": file, "line": line,
                       "end_line": line, "is_def": 0, "sig": ch.displayname, "psig": canonical_params(ch)}
                parent = ch.semantic_parent
                if parent is not None and parent.kind in CLASS_KINDS:
                    rec["class_usr"] = parent.get_usr()
                if k == K.CXX_METHOD and ch.is_virtual_method():
                    rec["virtual"] = 2 if ch.is_pure_virtual_method() else 1
                self.symbols[usr] = rec
            if k == K.CXX_METHOD and ch.is_virtual_method():
                for base in _overridden(ch):
                    self.edges.add(("override", usr, base, file, line, ""))
            return []

        if k in CLASS_KINDS and ch.is_definition():
            usr = ch.get_usr()
            file, line = self.site(ch)
            bases = []
            for b in ch.get_children():
                if b.kind == K.CXX_BASE_SPECIFIER:
                    ref = b.referenced
                    bases.append({"usr": ref.get_usr() if ref is not None else "",
                                  "name": qname(ref) if ref is not None else b.spelling})
            self.classes[usr] = {"usr": usr, "name": qname(ch), "file": file, "line": line,
                                 "end_line": ch.extent.end.line, "bases": bases}
            return [(x, new) for x in ch.get_children()]

        if k == K.LAMBDA_EXPR:
            owner = self.enclosing(ctx, ch)
            file, line = self.site(ch)
            usr = "%s@lambda:%s:%d:%d" % (owner, file, line, ch.location.column)
            owner_name = self.symbols.get(owner, {}).get("name", owner)
            # The parameter types, so a profile can tell this lambda from its siblings:
            # GCC numbers a function's lambdas #1, #2, ... PER SIGNATURE, not overall.
            params = [c.type.get_canonical().spelling for c in ch.get_children() if c.kind == K.PARM_DECL]
            lsig = "lambda(%s)" % ", ".join(params)
            var = ctx["var"]
            # Named only when the variable HOLDS the lambda: its type is the
            # closure (`auto f = [..]`) or a std::function. `std::thread t([..])`
            # passes the lambda to a constructor; t is a thread, not the lambda.
            if var and (var[1] == (ch.type.spelling or "") or "function<" in var[1]):
                # `auto callToolLoopTurn = [&](...) {...}`: findable by the variable's name
                self.add_symbol(usr, "%s::%s" % (owner_name, var[0]), "lambda", ch, {"sig": lsig})
            else:
                self.add_symbol(usr, "%s::<lambda@%s:%d>" % (owner_name, file, line), "lambda", ch, {"sig": lsig})
            self.lambdas[(file, line, ch.location.column)] = usr
            self.edge("defines", owner, usr, ch)
            if ctx["thread"]:
                self.edges.add(("thread", owner, usr) + ctx["thread"])
            if ctx["dispatch"]:
                self.edge("dispatch", ctx["dispatch"], usr, ch)
            new.update(fn=usr, callee=None, thread=None, var=None)
            return [(x, new) for x in ch.get_children()]

        if k == K.CALL_EXPR:
            tgt = ch.referenced
            if tgt is None and THREAD_TYPES.match(ch.type.spelling or ""):
                # `std::thread(...)` whose arguments depend on a template
                # parameter: the constructor is not resolved, the type is.
                file, line = self.site(ch)
                if file is not None:
                    new["thread"] = (file, line, ch.type.spelling + " (template-dependent)")
                new["callee"] = None
                return [(x, new) for x in ch.get_children()]
            if tgt is not None and tgt.kind in FUNC_KINDS:
                src = self.enclosing(ctx, ch)
                dst = self.lambda_of(tgt) or self.note_target(tgt)
                name = qname(tgt)
                self.edge("call", src, dst, ch, "virtual" if tgt.kind == K.CXX_METHOD and tgt.is_virtual_method() else "")
                if ctx["dispatch"]:
                    self.edge("dispatch", ctx["dispatch"], dst, ch)
                new["callee"] = dst
                # Thread starts: what is passed in (a lambda or a function) runs on another thread.
                if THREAD_CALLEES.match(name) or (tgt.kind == K.CXX_METHOD and tgt.spelling in POOL_METHODS
                                                  and "pool" in name.lower()):
                    file, line = self.site(ch)
                    if file is not None:
                        new["thread"] = (file, line, name)
                if name.endswith("::registerExecutor") or name == "registerExecutor":
                    self.register_executor(ch)
                if tgt.spelling in ASSIGN_OPERATOR_FNS:
                    # overloaded assignment on a class type: atomic += n, string = s
                    target = next((a for a in ch.get_children() if not self.refers_to(a, tgt)), None)
                    self.record_write("write", src, target, ch, tgt.spelling[len("operator"):])
                elif tgt.kind == K.CXX_METHOD and tgt.spelling in MUTATING_METHODS:
                    callee = next(iter(ch.get_children()), None)
                    if callee is not None and callee.kind == K.MEMBER_REF_EXPR:
                        obj = next(iter(callee.get_children()), None)
                        self.record_write("mutate", src, obj, ch, tgt.spelling)
                if tgt.kind != K.CONSTRUCTOR:
                    new["var"] = None
            else:
                new["callee"] = None
                new["var"] = None
            return [(x, new) for x in ch.get_children()]

        if k in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
            tgt = ch.referenced
            if tgt is not None and tgt.kind in FUNC_KINDS:
                dst = self.lambda_of(tgt) or self.note_target(tgt)
                if dst != ctx["callee"]:
                    src = self.enclosing(ctx, ch)
                    self.edge("ref", src, dst, ch)
                    if ctx["thread"]:
                        self.edges.add(("thread", src, dst) + ctx["thread"])
            new["callee"] = None
            new["var"] = None
            return [(x, new) for x in ch.get_children()]

        if k == K.IF_STMT and ctx["in_call_method"]:
            kids = list(ch.get_children())
            name = self.dispatch_name(kids[0]) if kids else None
            if name:
                file, line = self.site(ch)
                key = "naab:%s.%s" % (ctx["in_call_method"], name)    # class usr resolved to module at merge
                body = kids[1] if len(kids) > 1 else ch
                self.dispatch_ranges[key] = {"file": file, "line": line, "end_line": body.extent.end.line,
                                             "class_usr": ctx["in_call_method"], "fn": name}
                out = [(kids[0], dict(new, dispatch=None))]
                if len(kids) > 1:
                    out.append((kids[1], dict(new, dispatch=key)))
                out.extend((x, dict(new, dispatch=None)) for x in kids[2:])
                return out
            return [(x, new) for x in kids]

        if k in (K.VAR_DECL, K.FIELD_DECL):
            new["var"] = (ch.spelling, ch.type.get_canonical().spelling or "")
        elif k not in VAR_KEEP:
            new["var"] = None
        if k in (K.BINARY_OPERATOR, K.COMPOUND_ASSIGNMENT_OPERATOR, K.UNARY_OPERATOR) and ctx["fn"] is not None:
            op = operator_spelling(ch)
            if op in ASSIGN_OPS or op in INCDEC_OPS:
                self.record_write("write", ctx["fn"], next(iter(ch.get_children()), None), ch, op)
        if k not in WRAPPERS:
            new["callee"] = None
        return [(x, new) for x in ch.get_children()]

    # -- writes ----------------------------------------------------------------
    @staticmethod
    def refers_to(node, fn):
        """True when `node` is (a wrapper around) a reference to the function fn itself."""
        d = node
        while d is not None and d.kind in WRAPPERS:
            d = next(iter(d.get_children()), None)
        return d is not None and d.kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR) and d.referenced is not None \
            and d.referenced.get_usr() == fn.get_usr()

    def written_decl(self, expr):
        """The field or non-local variable an lvalue expression names: a.b.c -> c,
        arr[i] -> arr, map[k] -> map, (*p).x -> x. None for locals and temporaries."""
        d, hops = expr, 0
        while d is not None and hops < 12:
            hops += 1
            if d.kind in (K.MEMBER_REF_EXPR, K.DECL_REF_EXPR):
                ref = d.referenced
                if ref is None:
                    return None
                if ref.kind == K.FIELD_DECL:
                    return ref
                if ref.kind == K.VAR_DECL:
                    parent = ref.semantic_parent
                    local = parent is not None and (parent.kind in FUNC_KINDS or parent.kind == K.LAMBDA_EXPR)
                    if local and ref.storage_class != ci.StorageClass.STATIC:
                        return None
                    return ref
                return None
            if d.kind == K.CALL_EXPR and d.spelling in ("operator[]", "operator*", "operator->"):
                op_fn = d.referenced
                d = next((a for a in d.get_children() if op_fn is None or not self.refers_to(a, op_fn)), None)
                continue
            d = next(iter(d.get_children()), None)
        return None

    def record_write(self, kind, src, lhs, site, op):
        if src is None or lhs is None:
            return
        ref = self.written_decl(lhs)
        if ref is None or ref.location.file is None:
            return
        usr = ref.get_usr()
        if usr not in self.symbols:
            file = self.rel(ref.location.file.name) if self.in_repo(ref) else ref.location.file.name
            self.symbols[usr] = {"usr": usr, "name": qname(ref), "kind": ref.kind.name.lower(), "file": file,
                                 "line": ref.location.line, "end_line": ref.location.line, "is_def": 1,
                                 "sig": ref.type.spelling}
        self.edge(kind, src, usr, site, op)

    # -- the pre-dispatch allowlist ------------------------------------------------
    def read_allowlist(self, fn, usr):
        """Read `if (module == "m") { if (method == "a" || ...) return "read"; ... return "write"; }`.
        Returns {"usr", "file", "line", "modules": {m: {"methods": {a: mode}, "default": mode}}},
        with "modules" empty when the shape is not recognised (reported, never guessed)."""
        params = [c.spelling for c in fn.get_children() if c.kind == K.PARM_DECL]
        file, line = self.site(fn)
        out = {"usr": usr, "file": file, "line": line, "modules": {}}
        if len(params) < 2:
            return out
        body = next((c for c in fn.get_children() if c.kind == K.COMPOUND_STMT), None)
        if body is None:
            return out
        for stmt in body.get_children():
            if stmt.kind != K.IF_STMT:
                continue
            kids = list(stmt.get_children())
            mod = self.compared_literals(kids[0], params[0]) if kids else []
            if len(mod) != 1 or len(kids) < 2:
                continue
            entry = {"methods": {}, "default": ""}
            for inner in kids[1].get_children():
                if inner.kind == K.IF_STMT:
                    ik = list(inner.get_children())
                    names = self.compared_literals(ik[0], params[1]) if ik else []
                    mode = self.first_string(ik[1]) if len(ik) > 1 else None
                    for n in names:
                        if mode is not None:
                            entry["methods"][n] = mode
                elif inner.kind == K.RETURN_STMT:
                    entry["default"] = self.first_string(inner) or ""
            out["modules"][mod[0]] = entry
        return out

    @staticmethod
    def compared_literals(cond, param):
        """String literals compared with `param` by operator== in a condition."""
        names, uses_param = [], False
        for d in cond.walk_preorder():
            if d.kind == K.DECL_REF_EXPR and d.spelling == param:
                uses_param = True
            elif d.kind == K.STRING_LITERAL:
                names.append(d.spelling[1:-1] if d.spelling.startswith('"') else d.spelling)
        return names if uses_param else []

    # -- small pattern readers -----------------------------------------------
    @staticmethod
    def first_string(c):
        for d in c.walk_preorder():
            if d.kind == K.STRING_LITERAL:
                s = d.spelling
                return s[1:-1] if s.startswith('"') and s.endswith('"') else s
        return None

    def dispatch_name(self, cond):
        """`if (function_name == "read")` -> "read" when compared against a parameter."""
        has_param = has_eq = False
        lit = None
        for d in cond.walk_preorder():
            if d.kind == K.CALL_EXPR and d.spelling == "operator==":
                has_eq = True
            elif d.kind == K.DECL_REF_EXPR and d.referenced is not None and d.referenced.kind == K.PARM_DECL:
                has_param = True
            elif d.kind == K.STRING_LITERAL and lit is None:
                lit = d.spelling[1:-1] if d.spelling.startswith('"') else d.spelling
        if has_eq and has_param and lit and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", lit):
            return lit
        return None

    def register_executor(self, call):
        args = list(call.get_arguments())
        if len(args) < 2:
            return
        lang = self.first_string(args[0])
        # The argument's own type is after conversion to the parameter
        # (unique_ptr<Executor>); the registered class is the innermost
        # unique_ptr<X> inside it (make_unique<X>(), a moved local).
        m = None
        for d in args[1].walk_preorder():
            # canonical: libstdc++ spells make_unique's type __detail::__unique_ptr_t<X>
            hit = re.search(r"unique_ptr<\s*(?:class |struct )?([A-Za-z_:][A-Za-z0-9_:]*)",
                            d.type.get_canonical().spelling or "")
            if hit:
                m = hit
        if lang and m:
            file, line = self.site(call)
            self.edges.add(("dispatch", "lang:" + lang, "codemap:class:" + m.group(1),
                            file, line, "registerExecutor"))
            self.symbols.setdefault("lang:" + lang, {"usr": "lang:" + lang, "name": "<<" + lang + ">> executor",
                                                     "kind": "language", "file": file, "line": line,
                                                     "end_line": line, "is_def": 1})

    def result(self):
        includes = set()
        for inc in self.tu.get_includes():
            src, dst = inc.source, inc.include
            if src is None or dst is None:
                continue
            s, d = os.path.abspath(src.name), os.path.abspath(dst.name)
            if s.startswith(self.repo + os.sep) and d.startswith(self.repo + os.sep) \
                    and (os.sep + "external" + os.sep) not in d:
                includes.add((self.rel(s), self.rel(d)))
        return {"symbols": list(self.symbols.values()), "classes": list(self.classes.values()),
                "edges": [list(e) for e in sorted(self.edges)], "includes": sorted(list(p) for p in includes),
                "modules": self.modules, "dispatch_ranges": self.dispatch_ranges,
                "predispatch": self.predispatch}


def extract_tu(repo, cdb_dir, source):
    """Parse one source file; return the shard dict (or raise)."""
    cdb = ci.CompilationDatabase.fromDirectory(cdb_dir)
    cmds = cdb.getCompileCommands(source)
    if not cmds:
        raise RuntimeError("no compile command for " + source)
    cmd = cmds[0]
    args = compile_args(cmd, source)
    cwd = os.getcwd()
    os.chdir(cmd.directory)
    try:
        t0 = time.time()
        tu = ci.Index.create().parse(source, args=args)
        parse_s = time.time() - t0
        errors = [str(d) for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error][:5]
        data = Extractor(repo, tu, source).run()
    finally:
        os.chdir(cwd)
    deps = {}
    for f, _ in data["includes"]:
        deps[f] = None
    for _, f in data["includes"]:
        deps[f] = None
    deps[os.path.relpath(source, repo)] = None
    for f in list(deps):
        try:
            st = os.stat(os.path.join(repo, f))
            deps[f] = [st.st_mtime_ns, st.st_size]
        except OSError:
            deps[f] = None
    data.update({"source": os.path.relpath(source, repo), "args_hash": args_hash(args),
                 "deps": deps, "parse_seconds": round(parse_s, 2), "errors": errors})
    return data


if __name__ == "__main__":
    # extract.py REPO CDB_DIR SOURCE -> shard JSON on stdout (debugging aid)
    json.dump(extract_tu(os.path.abspath(sys.argv[1]), sys.argv[2], os.path.abspath(sys.argv[3])), sys.stdout)
