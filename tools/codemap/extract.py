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
                    "in_call_method": None}
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
            extra = {"sig": ch.displayname}
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
            return [(x, new) for x in ch.get_children()]

        if k in FUNC_KINDS:
            # A declaration: record it (a pure virtual has nothing else) and the
            # virtual relationship. A definition seen anywhere replaces it at merge.
            usr = ch.get_usr()
            file, line = self.site(ch)
            if usr and file is not None and usr not in self.symbols:
                rec = {"usr": usr, "name": qname(ch), "kind": k.name.lower(), "file": file, "line": line,
                       "end_line": line, "is_def": 0, "sig": ch.displayname}
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
            self.add_symbol(usr, "%s::<lambda@%s:%d>" % (owner_name, file, line), "lambda", ch)
            self.lambdas[(file, line, ch.location.column)] = usr
            self.edge("defines", owner, usr, ch)
            if ctx["thread"]:
                self.edges.add(("thread", owner, usr) + ctx["thread"])
            if ctx["dispatch"]:
                self.edge("dispatch", ctx["dispatch"], usr, ch)
            new.update(fn=usr, callee=None, thread=None)
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
            else:
                new["callee"] = None
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

        if k not in WRAPPERS:
            new["callee"] = None
        return [(x, new) for x in ch.get_children()]

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
                "modules": self.modules, "dispatch_ranges": self.dispatch_ranges}


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
