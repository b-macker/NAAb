// NAAb -- the static information-flow analysis behind `naab-gov taint FILE`
// (Bundle 12 PR 2).
//
// The run-time taint check judges the VALUE that reaches a sink: is the data
// itself derived from a source? It cannot see an IMPLICIT flow -- a sink reached
// only on a path tainted data chose:
//
//     if t == "x" { http.get(A) } else { http.get(B) }
//
// sends no tainted byte, yet tells the receiver one bit of t, and a loop over t
// sends all of it. This analysis walks the program statically, with the
// expression rule both engines share (naab/taint_rule.h) for every value, and
// adds what that rule cannot judge alone: statements. A branch, loop, short-
// circuit, match arm or catch whose choice depends on tainted data runs under a
// tainted CONTROL label; what it assigns is tainted, the sinks it calls are
// control flows, and an early exit under it (return, break, continue, throw)
// taints everything after it in the function or loop.
//
// Report only (the owner's decision, 2026-11-04): it refuses nothing. It is
// meant to measure how often such flows appear, and how often the analysis is
// wrong, before any of it is enforced.
//
// What is a source, a sanitizer or a sink is asked of the engine
// (GovernanceEngine::isTaintSource / isSanitizer / isTaintSink), so a report and
// a run under one govern.json decide those names alike.
//
// Sound in the over-approximating direction where it can be: a construct it
// cannot follow is counted as a blind spot, never silently passed. Termination-
// insensitive: whether a loop that depends on tainted data ends is not counted.
#pragma once

#include "naab/ast.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace naab {
namespace taint {

// The engine's decisions about a name (see above).
struct FlowPolicy {
    std::function<bool(const std::string&)> is_source;
    std::function<bool(const std::string&)> is_sanitizer;
    std::function<bool(const std::string&)> is_sink;
    // A `use` of a standard-library module (env, file, ...) is not an imported
    // NAAb module to follow: the stdlib's own registry decides.
    std::function<bool(const std::string&)> is_stdlib_module;
};

struct FlowFinding {
    // Value: the data reaching the sink is computed from a source -- an explicit
    // flow, the kind a run's taint check exists to judge (where a run misses
    // one, that is a gap in the run). Control: the data is not, but it was set,
    // or the sink was reached, only on a path tainted data chose -- an implicit
    // flow, which no run-time check judges.
    enum class Kind { Value, Control };
    Kind kind = Kind::Value;
    std::string sink;      // as taint_tracking.sinks names it: print, file.write, shell_exec
    int line = 0;
    std::string function;  // the enclosing function; "main" for the main block
    std::string origin;    // where the taint came from, e.g. "env.get at line 3"
    std::string via;       // the call that reached this function, "" at top level
};

struct FlowReport {
    std::vector<FlowFinding> findings;
    // What the analysis could not follow, counted by kind. A report with any
    // entry here says nothing about those constructs.
    std::map<std::string, int> blind_spots;
    int functions_analysed = 0;
    bool converged = true;  // false: recursion did not settle; blind_spots says so
};

FlowReport analyzeProgram(ast::Program& program, const FlowPolicy& policy);

const char* kindName(FlowFinding::Kind k);

}  // namespace taint
}  // namespace naab
