#pragma once
// The standard-library modules the VM binds before a program runs, so a
// program may call them without `use`. One list, read by the VM (its globals
// at construction, and the async-call copy that skips them) and by the type
// checker behind `naab-lang check`, which binds exactly these and each
// `use`/`import` alias (A188). Before, the checker kept its own hand-typed
// list: it rejected `use codegen` and passed `regex.matches(...)` without
// `use regex`, which fails on both engines.
//
// `string` is not here: it is a builtin function as well as a module, bound
// by the VM's builtins. The tree-walker's prelude is smaller
// (interpreter.cpp: array, string, dict, io, file, debug, bolo, env), a
// divergence recorded in A188; the tree-walker is refused under enforce.

namespace naab::stdlib {

inline constexpr const char* kVmPreludeModules[] = {
    "array", "dict", "io", "file", "debug", "bolo", "env",
    "math", "json", "http", "crypto", "time", "os"
};

}  // namespace naab::stdlib
