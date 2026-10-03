#!/usr/bin/env python3
"""Generate dumper.cpp: prints every field reachable from GovernanceRules.

Usage: gen_dumper.py records.json dumper.cpp

The dumper loads a config through GovernanceEngine::loadFromString() -- the
same loadFromJson() + enforceMinimumLevels() path loadFromFile() runs -- and
prints one "path<TAB>value" line per scalar field, so two configs can be
compared field by field at the point where the loader hands off to consumers.
"""
import json, re, sys
recs = json.load(open(sys.argv[1]))
# map full qualified names "naab::X" -> fields
full = {('naab::' + k if not k.startswith('naab::') else k): v for k, v in recs.items()}
root = 'naab::governance::GovernanceRules'
reach, todo = set(), [root]
names = sorted(full.keys(), key=len, reverse=True)
while todo:
    r = todo.pop()
    if r in reach: continue
    reach.add(r)
    for fname, qt, dt, init in full[r]:
        for n in names:
            if re.search(r'(?<![A-Za-z0-9_:])' + re.escape(n) + r'(?![A-Za-z0-9_])', dt):
                if n not in reach: todo.append(n)
order = sorted(reach)
out = []
out.append('#include "naab/governance.h"\n#include <iostream>\n#include <sstream>\n#include <fstream>\n#include <type_traits>\n#include <algorithm>\n')
out.append('using Out = std::ostream;\n')
for r in order:
    out.append(f'void dumpS(Out& o, const std::string& p, const {r}& v);')
out.append(r'''
template <class T, class = void> struct has_dumpS : std::false_type {};
template <class T> struct has_dumpS<T, std::void_t<decltype(dumpS(std::declval<Out&>(), std::declval<const std::string&>(), std::declval<const T&>()))>> : std::true_type {};
template <class T, class = void> struct is_iterable : std::false_type {};
template <class T> struct is_iterable<T, std::void_t<decltype(std::begin(std::declval<const T&>())), decltype(std::end(std::declval<const T&>()))>> : std::true_type {};
template <class T> struct is_pair : std::false_type {};
template <class A, class B> struct is_pair<std::pair<A,B>> : std::true_type {};
template <class T> struct is_optional : std::false_type {};
template <class A> struct is_optional<std::optional<A>> : std::true_type {};
template <class T> struct is_ordered_assoc : std::false_type {};
template <class K, class V, class C, class A> struct is_ordered_assoc<std::map<K,V,C,A>> : std::true_type {};
template <class K, class C, class A> struct is_ordered_assoc<std::set<K,C,A>> : std::true_type {};

template <class T> std::string scalar(const T& v) {
    std::ostringstream s;
    if constexpr (std::is_same_v<T, bool>) s << (v ? "true" : "false");
    else if constexpr (std::is_enum_v<T>) s << "enum:" << static_cast<long long>(v);
    else if constexpr (std::is_floating_point_v<T>) { s.precision(10); s << v; }
    else if constexpr (std::is_arithmetic_v<T>) s << +v;
    else if constexpr (std::is_same_v<T, std::string>) { s << '"'; for (char c : v) { if (c=='\n') s << "\\n"; else if (c=='\t') s << "\\t"; else s << c; } s << '"'; }
    else s << "<opaque>";
    return s.str();
}
template <class T> constexpr bool is_scalar_like = std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>;

template <class T> void dv(Out& o, const std::string& p, const T& v) {
    if constexpr (has_dumpS<T>::value) { dumpS(o, p, v); }
    else if constexpr (is_scalar_like<T>) { o << p << "\t" << scalar(v) << "\n"; }
    else if constexpr (is_optional<T>::value) { if (v) dv(o, p, *v); else o << p << "\t<nullopt>\n"; }
    else if constexpr (std::is_same_v<T, std::regex>) { o << p << "\t<regex>\n"; }
    else if constexpr (is_iterable<T>::value) {
        using E = std::decay_t<decltype(*std::begin(v))>;
        if constexpr (is_pair<E>::value) {
            using K = std::decay_t<typename E::first_type>;
            using V = std::decay_t<typename E::second_type>;
            std::vector<std::pair<std::string, const V*>> items;
            for (const auto& kv : v) items.emplace_back(scalar(kv.first), &kv.second);
            std::sort(items.begin(), items.end(), [](auto& a, auto& b){ return a.first < b.first; });
            o << p << ".#size\t" << items.size() << "\n";
            for (auto& [k, pv] : items) dv(o, p + "[" + k + "]", *pv);
        } else if constexpr (is_scalar_like<E>) {
            std::vector<std::string> items;
            for (const auto& e : v) items.push_back(scalar(e));
            if constexpr (!is_ordered_assoc<T>::value && !std::is_same_v<T, std::vector<E>> && !std::is_same_v<T, std::deque<E>>)
                std::sort(items.begin(), items.end());
            o << p << "\t[";
            for (size_t i = 0; i < items.size(); ++i) o << (i ? "," : "") << items[i];
            o << "]\n";
        } else {
            size_t i = 0;
            o << p << ".#size\t" << std::distance(std::begin(v), std::end(v)) << "\n";
            for (const auto& e : v) dv(o, p + "[" + std::to_string(i++) + "]", e);
        }
    }
    else { o << p << "\t<opaque>\n"; }
}
''')
for r in order:
    out.append(f'void dumpS(Out& o, const std::string& p, const {r}& v) {{')
    for fname, qt, dt, init in full[r]:
        out.append(f'    dv(o, p + ".{fname}", v.{fname});')
    out.append('}')
out.append(r'''
int main(int argc, char** argv) {
    std::ifstream f(argv[1]); std::stringstream ss; ss << f.rdbuf();
    naab::governance::GovernanceEngine e;
    bool ok = e.loadFromString(ss.str());
    std::cout << "LOAD\t" << (ok ? "ok" : "FAIL") << "\n";
    if (!ok) return 2;
    dumpS(std::cout, "R", e.getRules());
    return 0;
}
''')
open(sys.argv[2], 'w').write('\n'.join(out))
print(len(order), 'records reachable')
