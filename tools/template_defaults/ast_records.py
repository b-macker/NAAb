#!/usr/bin/env python3
"""Extract struct field lists from a clang JSON AST dump.

Input: the output of `clang++ -Xclang -ast-dump=json -Xclang -ast-dump-filter=naab::`
(a stream of concatenated JSON objects, one per matched declaration).
Output: records.json -- {qualified_struct_name: [[field, qualType, desugared, has_init], ...]}

The field list comes from the compiler, not from parsing governance.h by hand,
so nested structs, aliases and anonymous-namespace oddities resolve the way the
real build resolves them.
"""
import json
import sys


def main(ast_path, out_path):
    txt = open(ast_path, encoding="utf-8", errors="replace").read()
    dec = json.JSONDecoder()
    i, objs = 0, []
    while i < len(txt):
        while i < len(txt) and txt[i].isspace():
            i += 1
        if i >= len(txt):
            break
        o, i = dec.raw_decode(txt, i)
        objs.append(o)

    records = {}

    def walk(node, ns):
        kind = node.get("kind")
        if kind == "NamespaceDecl":
            ns = ns + [node.get("name", "")]
        if kind == "CXXRecordDecl" and node.get("completeDefinition") and node.get("name"):
            q = "::".join(ns + [node["name"]])
            fields = []
            for c in node.get("inner", []):
                if c.get("kind") == "FieldDecl":
                    t = c["type"].get("desugaredQualType", c["type"]["qualType"])
                    fields.append([c["name"], c["type"]["qualType"], t, "hasInClassInitializer" in c])
            if not records.get(q):
                records[q] = fields
            for c in node.get("inner", []):
                walk(c, ns + [node["name"]])
            return
        for c in node.get("inner", []):
            walk(c, ns)

    for o in objs:
        walk(o, [])
    json.dump(records, open(out_path, "w"), indent=1)
    print(f"{len(records)} records")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
