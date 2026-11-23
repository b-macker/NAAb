#pragma once
// Text as an evidence writer records it: valid UTF-8 (A207, Bundle 20 PR 5).
//
// Every evidence writer -- telemetry, the audit trail, the transcript, the
// JSON and SARIF reports, the run-history carry -- serialises with nlohmann,
// whose strict handler throws on a byte that is not UTF-8. A program's text,
// an operator's playbook or a file name can hold one (a Latin-1 comment), and
// the throw used to leave the run with none of its evidence: no telemetry
// line, no report, and an advisory-only run ending exit 1 instead of 0
// (observed). So no writer serialises text it has not passed through here.
//
// The rule is one replacement per invalid byte: a byte that does not start or
// continue a well-formed sequence (RFC 3629: no overlong forms, no surrogates,
// nothing above U+10FFFF) becomes U+FFFD and decoding resumes at the next
// byte. Valid text is returned unchanged, so a writer's output for valid text
// is what it always was, and a hash computed over the recorded text (a
// guidance delivery's recorded_sha256) agrees with the line because both use
// this one function.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace naab {
namespace text {

// True when every byte belongs to a well-formed UTF-8 sequence.
bool isValidUtf8(std::string_view s);

// `s` with each invalid byte replaced by U+FFFD (EF BF BD). Unchanged when
// `s` is valid.
std::string validUtf8(std::string_view s);

// Every string value and object key in `j` (an nlohmann::json; a template so
// this header carries no JSON include), recursively, through validUtf8().
// Changes nothing in a document that is already valid. A key that is not
// UTF-8 is renamed; when its replacement already names another key, the
// existing value stays (nothing a writer builds has two keys that differ only
// in invalid bytes).
template <class Json>
void makeValidUtf8(Json& j) {
    if (j.is_string()) {
        const auto& s = j.template get_ref<const std::string&>();
        if (!isValidUtf8(s)) j = validUtf8(s);
        return;
    }
    if (j.is_array()) {
        for (auto& e : j) makeValidUtf8(e);
        return;
    }
    if (!j.is_object()) return;
    std::vector<std::string> bad_keys;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!isValidUtf8(it.key())) bad_keys.push_back(it.key());
        makeValidUtf8(it.value());
    }
    for (const auto& k : bad_keys) {
        Json v = std::move(j[k]);
        j.erase(k);
        const std::string fixed = validUtf8(k);
        if (!j.contains(fixed)) j[fixed] = std::move(v);
    }
}

// The one serialisation for an evidence line or report: makeValidUtf8(), then
// dump. `indent` is nlohmann's (-1 compact). The dump uses the replacing
// handler as well, so a value that reached it some other way still cannot
// throw; after makeValidUtf8() that handler has nothing to replace.
template <class Json>
std::string dumpEvidence(Json& j, int indent = -1) {
    makeValidUtf8(j);
    return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

// The one serialisation for a request a model receives (A209): the same
// repair, so one stray byte no longer ends the run before anything is sent
// (`[json.exception.type_error.316]`, exit 1). The body's request_sha256 is
// taken over this output, the bytes actually sent.
//
// This is the BACKSTOP, not the rule. Text a scan judges -- a prompt, a tool's
// result, a guidance section -- is made valid where it ENTERS the request,
// before its scan, so the scan judges the bytes the request carries: under
// byte-wise matching (rx.cpp runs RE2 as Latin-1) one invalid byte and its
// three-byte U+FFFD can decide a length-bounded pattern differently. What
// reaches dumpRequest() unrepaired is text no scan reads.
template <class Json>
std::string dumpRequest(Json& j) {
    return dumpEvidence(j);
}

// The first `max` bytes of `s`, shortened to the last character boundary at
// or before them, so a cut never splits a multi-byte character (the cut of a
// valid string is valid). `s` unchanged when it is no longer than `max`.
std::string truncateUtf8(std::string_view s, size_t max);

}  // namespace text
}  // namespace naab
