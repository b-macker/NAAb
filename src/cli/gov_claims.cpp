// naab-gov claims FILE|- [--tools NAME,NAME...] [--json]
//
// Bundle 20 PR 5. What the claim reader (include/naab/claim_reader.h) makes of
// a reply: each claim it reads -- its kind, whether it says the thing worked,
// whether it was negated, its byte span and the word -- and, with --tools,
// what the reply says about each tool, as S19 attributes it. Report only: it
// runs nothing, loads no govern.json and decides nothing. The engine reads
// replies with the same function, so this is how an operator, or a test,
// checks a reading.
//
// Exit: 0 a report (claims or none), 4 usage or an unreadable file.

#include "naab/claim_reader.h"
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace cl = naab::claims;

namespace {

// A claim word is letters and apostrophes; anything else is shown as '?'.
std::string shown(const std::string& s) {
    std::string out;
    for (char ch : s.substr(0, 40)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back((std::isalpha(c) || c == '\'') ? ch : '?');
    }
    return out;
}

std::string toolVerdict(const cl::ToolClaims& t) {
    if (!t.mentioned) return "not_mentioned";
    if (t.positive && t.negative) return "mixed";
    if (t.positive) return "worked";
    if (t.negative) return "did_not";
    return "no_claim";
}

}  // namespace

int cmdClaims(const std::vector<std::string>& args) {
    std::string path;
    std::vector<std::string> tools;
    bool as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") { as_json = true; continue; }
        if (a == "--tools") {
            if (i + 1 >= args.size()) {
                std::cerr << "naab-gov claims: --tools needs a comma-separated list of names\n";
                return 4;
            }
            std::stringstream ss(args[++i]);
            std::string t;
            while (std::getline(ss, t, ','))
                if (!t.empty()) tools.push_back(t);
            continue;
        }
        if (!a.empty() && a[0] == '-' && a != "-") {
            std::cerr << "naab-gov claims: unknown option " << a << "\n";
            return 4;
        }
        if (!path.empty()) {
            std::cerr << "naab-gov claims: one input, a file or - for stdin\n";
            return 4;
        }
        path = a;
    }
    if (path.empty()) {
        std::cerr << "Usage: naab-gov claims FILE|- [--tools NAME,NAME...] [--json]\n"
                     "  What the claim reader makes of a reply (report only).\n";
        return 4;
    }
    std::string text;
    if (path == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "naab-gov claims: cannot read " << path << "\n";
            return 4;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // The same bound the transcript reader applies to one reply.
    if (text.size() > 16u * 1024 * 1024) {
        std::cerr << "naab-gov claims: input larger than 16 MiB\n";
        return 4;
    }

    const cl::ClaimReading r = cl::readClaims(text);
    if (as_json) {
        json out;
        out["claims"] = json::array();
        for (const auto& c : r.claims) {
            out["claims"].push_back({{"kind", cl::kindName(c.kind)},
                                     {"positive", c.positive},
                                     {"negated", c.negated},
                                     {"span", std::to_string(c.begin) + "-" + std::to_string(c.end)},
                                     {"sentence", c.sentence},
                                     {"word", shown(text.substr(c.begin, c.end - c.begin))}});
        }
        out["only_positive"] = cl::onlyPositiveClaims(r);
        out["tools"] = json::object();
        for (const auto& t : tools) out["tools"][t] = toolVerdict(cl::claimsAboutTool(r, t, tools));
        std::cout << out.dump(2, ' ', true, json::error_handler_t::replace) << "\n";
        return 0;
    }
    std::cout << "claims: " << r.claims.size() << "\n";
    for (const auto& c : r.claims) {
        std::cout << "  " << cl::kindName(c.kind) << (c.positive ? " +" : " -")
                  << (c.negated ? " negated" : "") << "  " << c.begin << "-" << c.end
                  << "  \"" << shown(text.substr(c.begin, c.end - c.begin)) << "\"\n";
    }
    std::cout << "only positive: " << (cl::onlyPositiveClaims(r) ? "yes" : "no") << "\n";
    for (const auto& t : tools)
        std::cout << "tool " << t << ": " << toolVerdict(cl::claimsAboutTool(r, t, tools)) << "\n";
    return 0;
}
