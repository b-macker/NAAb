// governance_checks.cpp — GovernanceEngine check implementations
// Extracted from governance.cpp lines 2718-5602

#include "naab/governance.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/vm.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#ifndef _WIN32
#  include <sys/file.h>
#endif
#include <fmt/core.h>

namespace naab {
namespace interpreter {
    extern thread_local Interpreter* g_current_interpreter;  // defined in interpreter.cpp
}
namespace governance {

// ============================================================================
// V3.0 New Check Implementations
// ============================================================================

// FIX 16: Strip string literal contents to prevent false positive pattern matches
// This prevents governance checks from triggering on code/paths inside strings
static std::string stripStringLiterals(const std::string& code) {
    std::string result;
    result.reserve(code.size());
    bool in_single = false, in_double = false, in_backtick = false;
    bool escaped = false;
    bool in_raw = false;  // Track Python raw strings — no escape processing
    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (escaped) { escaped = false; continue; }
        // C11 fix: track escapes in backtick strings too (JS template literals)
        // Skip escape processing in raw strings (r"..." / r'...')
        if (c == '\\' && (in_single || in_double || in_backtick) && !in_raw) { escaped = true; continue; }

        // S-10 fix: When not in a string, skip comment sequences so unmatched
        // quotes in comments (e.g., // don't) don't toggle string mode.
        // Comments are passed through to output for downstream checks that need them
        // (e.g., checkPlaceholders intentionally detects // TODO in comments).
        if (!in_single && !in_double && !in_backtick) {
            // Line comment: //
            if (c == '/' && i+1 < code.size() && code[i+1] == '/') {
                while (i < code.size() && code[i] != '\n') {
                    result += code[i];
                    i++;
                }
                if (i < code.size()) result += code[i]; // newline
                continue;
            }
            // Block comment: /* ... */
            if (c == '/' && i+1 < code.size() && code[i+1] == '*') {
                result += code[i]; i++; // /
                result += code[i]; i++; // *
                while (i < code.size()) {
                    if (code[i] == '*' && i+1 < code.size() && code[i+1] == '/') {
                        result += code[i]; i++; // *
                        result += code[i]; // /
                        break;
                    }
                    result += code[i];
                    i++;
                }
                continue;
            }
        }

        // V-GOV-001: consume Python/JS string prefixes (f, b, r, u and two-letter
        // combinations rb, br, rf, fr) before quote detection. Without this, the
        // prefix character is left in the output, allowing f"malicious_code()" to
        // leak the 'f' into the stripped result and fool pattern matchers.
        if (!in_single && !in_double && !in_backtick) {
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lc == 'f' || lc == 'b' || lc == 'r' || lc == 'u') {
                // Check for two-letter prefix (rb, br, rf, fr) followed by quote
                if (i + 2 < code.size()) {
                    char lc2 = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i+1])));
                    char q2 = code[i+2];
                    std::string two = {lc, lc2};
                    if ((two == "rb" || two == "br" || two == "rf" || two == "fr") &&
                        (q2 == '"' || q2 == '\'')) {
                        bool is_raw = (lc == 'r' || lc2 == 'r');
                        result += "  ";  // replace two-char prefix with spaces
                        i += 2;
                        c = q2; // fall through to quote handling below
                        // Re-run the quote handling with c = q2
                        if (c == '"' && !in_single && !in_backtick && !in_double
                            && i+2 < code.size() && code[i+1] == '"' && code[i+2] == '"') {
                            i += 3;
                            while (i+2 < code.size()) {
                                if (code[i] == '"' && code[i+1] == '"' && code[i+2] == '"') { i += 2; break; }
                                i++;
                            }
                            continue;
                        }
                        if (c == '\'' && !in_double && !in_backtick && !in_single
                            && i+2 < code.size() && code[i+1] == '\'' && code[i+2] == '\'') {
                            i += 3;
                            while (i+2 < code.size()) {
                                if (code[i] == '\'' && code[i+1] == '\'' && code[i+2] == '\'') { i += 2; break; }
                                i++;
                            }
                            continue;
                        }
                        if (c == '"')  { in_double  = true; in_raw = is_raw; continue; }
                        if (c == '\'') { in_single  = true; in_raw = is_raw; continue; }
                        continue;
                    }
                }
                // Check for single-letter prefix followed by quote
                if (i + 1 < code.size() && (code[i+1] == '"' || code[i+1] == '\'')) {
                    if (lc == 'r') in_raw = true;
                    result += ' ';  // replace prefix with space
                    i++;
                    c = code[i];
                    // fall through to standard quote handling
                }
            }
        }

        // EVA-6: Triple-double-quote: """...""" (Python docstrings)
        if (c == '"' && !in_single && !in_backtick && !in_double
            && i+2 < code.size() && code[i+1] == '"' && code[i+2] == '"') {
            i += 3;  // skip opening """
            while (i+2 < code.size()) {
                if (code[i] == '"' && code[i+1] == '"' && code[i+2] == '"') {
                    i += 2;  // will be incremented by loop
                    break;
                }
                i++;
            }
            continue;
        }
        // EVA-6: Triple-single-quote: '''...'''
        if (c == '\'' && !in_double && !in_backtick && !in_single
            && i+2 < code.size() && code[i+1] == '\'' && code[i+2] == '\'') {
            i += 3;
            while (i+2 < code.size()) {
                if (code[i] == '\'' && code[i+1] == '\'' && code[i+2] == '\'') {
                    i += 2;
                    break;
                }
                i++;
            }
            continue;
        }

        if (c == '"' && !in_single && !in_backtick) { in_double = !in_double; if (!in_double) in_raw = false; continue; }
        if (c == '\'' && !in_double && !in_backtick) { in_single = !in_single; if (!in_single) in_raw = false; continue; }
        if (c == '`' && !in_double && !in_single) { in_backtick = !in_backtick; continue; }
        if (!in_single && !in_double && !in_backtick) {
            result += c;
        }
    }
    return result;
}

// Normalize Unicode homoglyphs and zero-width characters to prevent evasion.
// Replaces confusable characters (Cyrillic/Greek lookalikes, fullwidth Latin,
// zero-width joiners/spaces) with their ASCII equivalents so regex patterns match.
// Map Mathematical Alphanumeric Symbols (U+1D400-U+1D7FF) to ASCII equivalents.
// These 4-byte UTF-8 codepoints include 13 alphabet variants (Bold, Italic,
// Bold Italic, Script, Bold Script, Fraktur, Bold Fraktur, Double-Struck,
// Sans-Serif, Sans-Serif Bold, Sans-Serif Italic, Sans-Serif Bold Italic,
// Monospace) plus digit variants, used to evade pattern matching.
static char mapMathAlpha(uint32_t cp) {
    // Mathematical alphabet ranges — each variant has 52 chars (A-Z then a-z)
    // with some holes for characters borrowed from other blocks
    struct AlphaRange { uint32_t upper_start; uint32_t lower_start; };
    static const AlphaRange ranges[] = {
        {0x1D400, 0x1D41A}, // Bold
        {0x1D434, 0x1D44E}, // Italic
        {0x1D468, 0x1D482}, // Bold Italic
        {0x1D49C, 0x1D4B6}, // Script
        {0x1D4D0, 0x1D4EA}, // Bold Script
        {0x1D504, 0x1D51E}, // Fraktur
        {0x1D538, 0x1D552}, // Double-Struck
        {0x1D56C, 0x1D586}, // Bold Fraktur
        {0x1D5A0, 0x1D5BA}, // Sans-Serif
        {0x1D5D4, 0x1D5EE}, // Sans-Serif Bold
        {0x1D608, 0x1D622}, // Sans-Serif Italic
        {0x1D63C, 0x1D656}, // Sans-Serif Bold Italic
        {0x1D670, 0x1D68A}, // Monospace
    };
    for (const auto& r : ranges) {
        if (cp >= r.upper_start && cp < r.upper_start + 26)
            return static_cast<char>('A' + (cp - r.upper_start));
        if (cp >= r.lower_start && cp < r.lower_start + 26)
            return static_cast<char>('a' + (cp - r.lower_start));
    }
    // Mathematical digit variants (Bold through Monospace, 5 ranges of 10)
    static const uint32_t digit_ranges[] = {
        0x1D7CE, // Bold digits
        0x1D7D8, // Double-Struck digits
        0x1D7E2, // Sans-Serif digits
        0x1D7EC, // Sans-Serif Bold digits
        0x1D7F6, // Monospace digits
    };
    for (uint32_t dr : digit_ranges) {
        if (cp >= dr && cp < dr + 10)
            return static_cast<char>('0' + (cp - dr));
    }
    return 0; // Not a mathematical alphanumeric
}

static std::string normalizeUnicode(const std::string& code) {
    std::string result;
    result.reserve(code.size());

    for (size_t i = 0; i < code.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(code[i]);

        // ASCII passthrough (fast path)
        if (c < 0x80) {
            result += code[i];
            continue;
        }

        // 2-byte UTF-8 sequences (0xC0-0xDF lead byte)
        if (c >= 0xC0 && c <= 0xDF && i + 1 < code.size()) {
            unsigned char c2 = static_cast<unsigned char>(code[i + 1]);
            uint16_t cp = static_cast<uint16_t>(((c & 0x1F) << 6) | (c2 & 0x3F));

            // Zero-width characters — remove entirely
            // U+00AD SOFT HYPHEN
            if (cp == 0x00AD) { i += 1; continue; }

            // Superscript digits in Latin-1 Supplement
            if (cp == 0x00B2) { result += '2'; i += 1; continue; } // ²
            if (cp == 0x00B3) { result += '3'; i += 1; continue; } // ³
            if (cp == 0x00B9) { result += '1'; i += 1; continue; } // ¹

            // Cyrillic confusables (U+0400-U+04FF)
            char replacement = 0;
            switch (cp) {
                case 0x0410: replacement = 'A'; break; // А
                case 0x0412: replacement = 'B'; break; // В
                case 0x0421: replacement = 'C'; break; // С
                case 0x0415: replacement = 'E'; break; // Е
                case 0x041D: replacement = 'H'; break; // Н
                case 0x041A: replacement = 'K'; break; // К
                case 0x041C: replacement = 'M'; break; // М
                case 0x041E: replacement = 'O'; break; // О
                case 0x0420: replacement = 'P'; break; // Р
                case 0x0422: replacement = 'T'; break; // Т
                case 0x0425: replacement = 'X'; break; // Х
                case 0x0430: replacement = 'a'; break; // а
                case 0x0441: replacement = 'c'; break; // с
                case 0x0435: replacement = 'e'; break; // е
                case 0x043E: replacement = 'o'; break; // о
                case 0x0440: replacement = 'p'; break; // р
                case 0x0455: replacement = 's'; break; // ѕ
                case 0x0445: replacement = 'x'; break; // х
                case 0x0443: replacement = 'y'; break; // у
                // Greek confusables
                case 0x0391: replacement = 'A'; break; // Α
                case 0x0392: replacement = 'B'; break; // Β
                case 0x0395: replacement = 'E'; break; // Ε
                case 0x0397: replacement = 'H'; break; // Η
                case 0x0399: replacement = 'I'; break; // Ι
                case 0x039A: replacement = 'K'; break; // Κ
                case 0x039C: replacement = 'M'; break; // Μ
                case 0x039D: replacement = 'N'; break; // Ν
                case 0x039F: replacement = 'O'; break; // Ο
                case 0x03A1: replacement = 'P'; break; // Ρ
                case 0x03A4: replacement = 'T'; break; // Τ
                case 0x03A5: replacement = 'Y'; break; // Υ
                case 0x03B1: replacement = 'a'; break; // α (marginal)
                case 0x03BF: replacement = 'o'; break; // ο
                default: break;
            }
            if (replacement) {
                result += replacement;
                i += 1;
                continue;
            }

            // Pass through other 2-byte sequences unchanged
            result += code[i];
            result += code[i + 1];
            i += 1;
            continue;
        }

        // 3-byte UTF-8 sequences (0xE0-0xEF lead byte)
        if (c >= 0xE0 && c <= 0xEF && i + 2 < code.size()) {
            unsigned char c2 = static_cast<unsigned char>(code[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(code[i + 2]);
            uint16_t cp = static_cast<uint16_t>(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));

            // Zero-width characters — remove entirely
            if (cp == 0x200B || cp == 0x200C || cp == 0x200D || // ZWS, ZWNJ, ZWJ
                cp == 0x2060 || cp == 0xFEFF) {                 // Word Joiner, BOM
                i += 2;
                continue;
            }

            // Superscript letters/digits (U+2070-U+207F)
            // U+2070=⁰, U+00B9=¹ (2-byte), U+00B2=² (2-byte), U+00B3=³ (2-byte)
            // U+2074=⁴, U+2075=⁵, U+2076=⁶, U+2077=⁷, U+2078=⁸, U+2079=⁹
            // U+207F=ⁿ
            if (cp >= 0x2074 && cp <= 0x2079) {
                result += static_cast<char>('0' + (cp - 0x2070));
                i += 2; continue;
            }
            if (cp == 0x2070) { result += '0'; i += 2; continue; }
            if (cp == 0x207F) { result += 'n'; i += 2; continue; }

            // Subscript letters/digits (U+2080-U+209C)
            // U+2080=₀..U+2089=₉, U+2090=ₐ, U+2091=ₑ, U+2092=ₒ,
            // U+2093=ₓ, U+2094=ₔ, U+2095=ₕ, U+2096=ₖ, U+2097=ₗ,
            // U+2098=ₘ, U+2099=ₙ, U+209A=ₚ, U+209B=ₛ, U+209C=ₜ
            if (cp >= 0x2080 && cp <= 0x2089) {
                result += static_cast<char>('0' + (cp - 0x2080));
                i += 2; continue;
            }
            {
                char sub_rep = 0;
                switch (cp) {
                    case 0x2090: sub_rep = 'a'; break; // ₐ
                    case 0x2091: sub_rep = 'e'; break; // ₑ
                    case 0x2092: sub_rep = 'o'; break; // ₒ
                    case 0x2093: sub_rep = 'x'; break; // ₓ
                    case 0x2094: sub_rep = 'e'; break; // ₔ (schwa → e)
                    case 0x2095: sub_rep = 'h'; break; // ₕ
                    case 0x2096: sub_rep = 'k'; break; // ₖ
                    case 0x2097: sub_rep = 'l'; break; // ₗ
                    case 0x2098: sub_rep = 'm'; break; // ₘ
                    case 0x2099: sub_rep = 'n'; break; // ₙ
                    case 0x209A: sub_rep = 'p'; break; // ₚ
                    case 0x209B: sub_rep = 's'; break; // ₛ
                    case 0x209C: sub_rep = 't'; break; // ₜ
                    default: break;
                }
                if (sub_rep) { result += sub_rep; i += 2; continue; }
            }

            // Fullwidth Latin letters (U+FF21-FF3A = A-Z, U+FF41-FF5A = a-z)
            if (cp >= 0xFF21 && cp <= 0xFF3A) {
                result += static_cast<char>('A' + (cp - 0xFF21));
                i += 2;
                continue;
            }
            if (cp >= 0xFF41 && cp <= 0xFF5A) {
                result += static_cast<char>('a' + (cp - 0xFF41));
                i += 2;
                continue;
            }
            // Fullwidth digits (U+FF10-FF19 = 0-9)
            if (cp >= 0xFF10 && cp <= 0xFF19) {
                result += static_cast<char>('0' + (cp - 0xFF10));
                i += 2;
                continue;
            }
            // Fullwidth punctuation
            if (cp == 0xFF08) { result += '('; i += 2; continue; } // （
            if (cp == 0xFF09) { result += ')'; i += 2; continue; } // ）
            if (cp == 0xFF0E) { result += '.'; i += 2; continue; } // ．
            if (cp == 0xFF3B) { result += '['; i += 2; continue; } // ［
            if (cp == 0xFF3D) { result += ']'; i += 2; continue; } // ］

            // Pass through other 3-byte sequences unchanged
            result += code[i];
            result += code[i + 1];
            result += code[i + 2];
            i += 2;
            continue;
        }

        // 4-byte UTF-8 sequences (0xF0-0xF7 lead byte)
        if (c >= 0xF0 && c <= 0xF7 && i + 3 < code.size()) {
            unsigned char c2 = static_cast<unsigned char>(code[i+1]);
            unsigned char c3 = static_cast<unsigned char>(code[i+2]);
            unsigned char c4 = static_cast<unsigned char>(code[i+3]);
            uint32_t cp = (static_cast<uint32_t>(c & 0x07) << 18) |
                          (static_cast<uint32_t>(c2 & 0x3F) << 12) |
                          (static_cast<uint32_t>(c3 & 0x3F) << 6) |
                          static_cast<uint32_t>(c4 & 0x3F);
            char mapped = mapMathAlpha(cp);
            if (mapped) {
                result += mapped;
                i += 3;
                continue;
            }
            // Pass through other 4-byte sequences unchanged
            result += code[i];
            result += code[i + 1];
            result += code[i + 2];
            result += code[i + 3];
            i += 3;
            continue;
        }

        // Invalid UTF-8 — pass through
        result += code[i];
    }
    return result;
}

// Normalize whitespace in code to prevent evasion via newlines/spaces in
// dotted access (os.\nsystem, os . system) and line continuations (os.\\\nsystem).
// Collapses whitespace around dots and removes Python line continuations.
static std::string normalizeWhitespace(const std::string& code) {
    // Pass 1: Remove line continuation (backslash + newline)
    std::string pass1;
    pass1.reserve(code.size());
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '\\' && i + 1 < code.size() && code[i + 1] == '\n') {
            i += 1; // skip both backslash and newline
            continue;
        }
        pass1 += code[i];
    }

    // Pass 2: Collapse whitespace (including newlines) around dots in
    // identifier.identifier patterns — e.g., "os . system" → "os.system"
    std::string result;
    result.reserve(pass1.size());
    for (size_t i = 0; i < pass1.size(); ++i) {
        // When we see a dot, collapse surrounding whitespace
        if (pass1[i] == '.') {
            // Remove trailing whitespace before the dot
            while (!result.empty() && (result.back() == ' ' || result.back() == '\t' ||
                                        result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
            result += '.';
            // Skip leading whitespace after the dot
            while (i + 1 < pass1.size() && (pass1[i + 1] == ' ' || pass1[i + 1] == '\t' ||
                                              pass1[i + 1] == '\n' || pass1[i + 1] == '\r')) {
                i++;
            }
            continue;
        }
        result += pass1[i];
    }
    return result;
}

// FIX 16: Strip comments (Python #, JS //, C /* */) for checks that shouldn't trigger on comments
static std::string stripComments(const std::string& code) {
    std::string result;
    result.reserve(code.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (size_t i = 0; i < code.size(); ++i) {
        if (in_line_comment) {
            if (code[i] == '\n') { in_line_comment = false; result += '\n'; }
            continue;
        }
        if (in_block_comment) {
            if (code[i] == '*' && i+1 < code.size() && code[i+1] == '/') {
                in_block_comment = false; i++;
            }
            continue;
        }
        if (code[i] == '/' && i+1 < code.size()) {
            if (code[i+1] == '/') { in_line_comment = true; continue; }
            if (code[i+1] == '*') { in_block_comment = true; i++; continue; }
        }
        if (code[i] == '#') { in_line_comment = true; continue; }
        result += code[i];
    }
    return result;
}

// FIX 18: Normalize language aliases for consistent governance matching
std::string normalizeLanguage(const std::string& language) {
    if (language == "bash" || language == "sh") return "shell";
    if (language == "golang") return "go";
    if (language == "cs") return "csharp";
    if (language == "ts") return "typescript";
    if (language == "c++") return "cpp";
    if (language == "rb") return "ruby";
    if (language == "js") return "javascript";
    if (language == "node") return "javascript";
    if (language == "py") return "python";
    return language;
}

// Helper: regex search against a list of patterns
// Process-wide compiled-regex cache for searchPatterns(). Patterns come from
// constant default tables and loaded config — a small, bounded set per
// process — so compiling each once amortizes the per-check regex cost.
// Capped so a pathological config cannot grow it unbounded; on overflow the
// caller compiles per-call as before. A cached nullptr marks a pattern that
// failed to compile (warned once, skipped thereafter).
enum class RegexCacheResult { HIT, BAD, FULL };
static RegexCacheResult cachedSearchRegex(const std::string& pat, bool case_insensitive,
                                          const std::regex** out) {
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, std::unique_ptr<std::regex>> cache;
    constexpr size_t kMaxEntries = 1024;

    std::string key = (case_insensitive ? "i:" : "s:") + pat;
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cache.find(key);
    if (it != cache.end()) {
        if (!it->second) return RegexCacheResult::BAD;
        *out = it->second.get();
        return RegexCacheResult::HIT;
    }
    if (cache.size() >= kMaxEntries) return RegexCacheResult::FULL;
    auto flags = std::regex::ECMAScript;
    if (case_insensitive) flags |= std::regex::icase;
    try {
        auto re = std::make_unique<std::regex>(pat, flags);
        *out = re.get();
        cache.emplace(std::move(key), std::move(re));
        return RegexCacheResult::HIT;
    } catch (const std::regex_error& e) {
        fprintf(stderr, "[governance] Warning: invalid regex pattern skipped: %s\n", e.what());
        cache.emplace(std::move(key), nullptr);
        return RegexCacheResult::BAD;
    }
}

static std::string searchPatterns(const std::string& code,
    const std::vector<std::string>& patterns, bool case_insensitive = true) {
    for (const auto& pat : patterns) {
        try {
            const std::regex* re = nullptr;
            RegexCacheResult cr = cachedSearchRegex(pat, case_insensitive, &re);
            if (cr == RegexCacheResult::BAD) continue;  // warned once at compile
            std::regex local_re;
            if (cr == RegexCacheResult::FULL) {
                auto flags = std::regex::ECMAScript;
                if (case_insensitive) flags |= std::regex::icase;
                local_re = std::regex(pat, flags);
                re = &local_re;
            }
            std::smatch match;
            if (std::regex_search(code, match, *re)) {
                return match[0].str();
            }
        } catch (const std::regex_error& e) {
            fprintf(stderr, "[governance] Warning: invalid regex pattern skipped: %s\n", e.what());
        } catch (const std::bad_alloc&) {
            // Memory exhausted during governance check — fail-closed
            throw GovernanceHardError(
                "Governance: memory exhausted during security check — execution halted.\n"
                "  This is a fail-safe: governance cannot verify safety under memory pressure.\n"
                "  Increase available memory or reduce RLIMIT_AS if set.\n"
            );
        }
    }
    return "";
}

// ─── Dangerous Alias Expansion ──────────────────────────────────────────────
// Detects variable aliasing of dangerous functions and expands alias calls
// so that downstream pattern-based checks catch indirect invocations.
//
//   s = os.system        →  alias: s → os.system
//   s("rm")              →  expanded to: os.system("rm")
//
// Handles: direct assignment, Python from-import (single/multi/star/as),
// module aliasing (import X as Y), and transitive chains (b = a = os.system).

struct DangerousAliasRef {
    std::string pattern;     // Regex for the function reference
    std::string canonical;   // Plain-text canonical form for expansion
    bool builtin;            // true = bare name (check not preceded by '.')
};

// Modules containing dangerous functions — used for module aliasing and star imports
static const std::unordered_map<std::string, std::vector<std::string>> DANGEROUS_MODULE_FUNCS = {
    {"os",         {"system", "popen"}},
    {"subprocess", {"call", "Popen", "run", "check_output", "check_call"}},
    {"pickle",     {"load", "loads"}},
    {"ctypes",     {"CDLL", "cdll"}},
    {"yaml",       {"load"}},
    {"importlib",  {"import_module"}},
};

static std::string expandDangerousAliases(const std::string& language,
                                           const std::string& code) {
    static const std::vector<DangerousAliasRef> PYTHON_REFS = {
        // Module-qualified dangerous functions
        {"os\\.system",               "os.system",               false},
        {"os\\.popen",                "os.popen",                false},
        {"subprocess\\.call",         "subprocess.call",         false},
        {"subprocess\\.Popen",        "subprocess.Popen",        false},
        {"subprocess\\.run",          "subprocess.run",          false},
        {"subprocess\\.check_output", "subprocess.check_output", false},
        {"subprocess\\.check_call",   "subprocess.check_call",   false},
        {"pickle\\.load(?!s)",        "pickle.load",             false},
        {"pickle\\.loads",            "pickle.loads",            false},
        {"ctypes\\.CDLL",             "ctypes.CDLL",             false},
        {"ctypes\\.cdll",             "ctypes.cdll",             false},
        {"yaml\\.load",               "yaml.load",               false},
        {"importlib\\.import_module",  "importlib.import_module", false},
        // Bare dangerous builtins
        {"eval",                      "eval",                    true},
        {"exec",                      "exec",                    true},
        {"__import__",                "__import__",              true},
        {"getattr",                   "getattr",                 true},
    };

    static const std::vector<DangerousAliasRef> JS_REFS = {
        {"eval",     "eval",     true},
        {"Function", "Function", true},
    };

    static const std::vector<DangerousAliasRef> GO_REFS = {
        {"exec\\.Command", "exec.Command", false},
    };

    static const std::vector<DangerousAliasRef> RUST_REFS = {
        {"Command::new", "Command::new", false},
    };

    static const std::vector<DangerousAliasRef> RUBY_REFS = {
        {"system", "system", true},
        {"exec",   "exec",   true},
        {"eval",   "eval",   true},
    };

    static const std::vector<DangerousAliasRef> PHP_REFS = {
        {"system",     "system",     true},
        {"exec",       "exec",       true},
        {"shell_exec", "shell_exec", true},
        {"passthru",   "passthru",   true},
        {"eval",       "eval",       true},
    };

    // M9 fix: normalize language aliases before REFS table lookup
    // so "js", "bash", "golang" etc. resolve correctly
    std::string lang = normalizeLanguage(language);

    const std::vector<DangerousAliasRef>* refs = nullptr;
    if (lang == "python")          refs = &PYTHON_REFS;
    else if (lang == "javascript") refs = &JS_REFS;
    else if (lang == "go")         refs = &GO_REFS;
    else if (lang == "rust")       refs = &RUST_REFS;
    else if (lang == "ruby")       refs = &RUBY_REFS;
    else if (lang == "php")        refs = &PHP_REFS;
    else return code;

    std::string assign_op = (language == "go" || language == "golang") ? ":?=" : "=";

    // Function aliases: alias → canonical (e.g., "s" → "os.system")
    std::unordered_map<std::string, std::string> aliases;

    // ── Phase 0: Module aliasing (import X as Y) — expand first ─────────
    // Module aliases must be resolved before function alias collection so that
    // `import subprocess as sp; fn = sp.Popen` correctly recognizes sp.Popen.
    std::string working = code;
    if (language == "python") {
        std::unordered_map<std::string, std::string> module_aliases;
        try {
            std::regex re("\\bimport\\s+(\\w+(?:\\.\\w+)*)\\s+as\\s+(\\w+)",
                          std::regex::ECMAScript);
            auto begin = std::sregex_iterator(code.begin(), code.end(), re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                std::string mod = (*m)[1].str();
                std::string alias = (*m)[2].str();
                if (DANGEROUS_MODULE_FUNCS.count(mod)) {
                    module_aliases[alias] = mod;
                }
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }

        // Expand module aliases: alias.func → module.func for dangerous funcs
        for (const auto& kv : module_aliases) {
            auto mod_it = DANGEROUS_MODULE_FUNCS.find(kv.second);
            if (mod_it == DANGEROUS_MODULE_FUNCS.end()) continue;
            for (const auto& func : mod_it->second) {
                try {
                    std::string pat = "\\b" + kv.first + "\\." + func + "\\b";
                    std::regex re(pat, std::regex::ECMAScript);
                    working = std::regex_replace(working, re, kv.second + "." + func);
                } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
            }
        }
    }

    // ── Phase 1: Direct assignment aliases ──────────────────────────────
    // Pattern: identifier = dangerous_ref (NOT followed by open paren → reference, not call)
    // Runs on module-expanded code so `fn = sp.Popen` → `fn = subprocess.Popen` is matched.
    for (const auto& ref : *refs) {
        std::string pat = "(\\w+)\\s*" + assign_op + "\\s*\\b(" +
                          ref.pattern + ")\\b(?!\\s*\\()";
        try {
            std::regex re(pat, std::regex::ECMAScript);
            auto begin = std::sregex_iterator(working.begin(), working.end(), re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                auto lhs_pos = static_cast<size_t>((*m).position(1));
                if (lhs_pos > 0 && working[lhs_pos - 1] == '.') continue;

                if (ref.builtin) {
                    auto rhs_pos = static_cast<size_t>((*m).position(2));
                    if (rhs_pos > 0 && working[rhs_pos - 1] == '.') continue;
                }

                std::string alias = (*m)[1].str();
                if (alias != ref.canonical) {
                    aliases[alias] = ref.canonical;
                }
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    // ── Phase 1c: Dict/list indirection ─────────────────────────────────
    // Detect two patterns:
    //   container[key] = dangerous_ref  (subscript assignment)
    //   container = [...dangerous_ref...]  (list literal containing ref)
    // Expand: container[...](  → canonical(
    std::unordered_map<std::string, std::string> dict_aliases;  // container → canonical

    for (const auto& ref : *refs) {
        // Subscript assignment: d["key"] = os.system
        std::string sub_pat = "(\\w+)\\s*\\[[^\\]]*\\]\\s*" + assign_op + "\\s*\\b(" +
                              ref.pattern + ")\\b(?!\\s*\\()";
        try {
            std::regex re(sub_pat, std::regex::ECMAScript);
            auto begin = std::sregex_iterator(working.begin(), working.end(), re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                auto lhs_pos = static_cast<size_t>((*m).position(1));
                if (lhs_pos > 0 && working[lhs_pos - 1] == '.') continue;

                if (ref.builtin) {
                    auto rhs_pos = static_cast<size_t>((*m).position(2));
                    if (rhs_pos > 0 && working[rhs_pos - 1] == '.') continue;
                }

                dict_aliases[(*m)[1].str()] = ref.canonical;
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }

        // List literal: fns = [os.system] or fns = [x, os.system, y]
        std::string list_pat = "(\\w+)\\s*" + assign_op + "\\s*\\[[^\\]]*\\b(" +
                               ref.pattern + ")\\b(?!\\s*\\()[^\\]]*\\]";
        try {
            std::regex re(list_pat, std::regex::ECMAScript);
            auto begin = std::sregex_iterator(working.begin(), working.end(), re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                auto lhs_pos = static_cast<size_t>((*m).position(1));
                if (lhs_pos > 0 && working[lhs_pos - 1] == '.') continue;

                if (ref.builtin) {
                    auto rhs_pos = static_cast<size_t>((*m).position(2));
                    if (rhs_pos > 0 && working[rhs_pos - 1] == '.') continue;
                }

                dict_aliases[(*m)[1].str()] = ref.canonical;
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    // ── Phase 2: Python from-import patterns ────────────────────────────
    if (language == "python") {
        static const std::unordered_map<std::string, std::string> IMPORT_MAP = {
            {"os.system",               "os.system"},
            {"os.popen",                "os.popen"},
            {"subprocess.call",         "subprocess.call"},
            {"subprocess.Popen",        "subprocess.Popen"},
            {"subprocess.run",          "subprocess.run"},
            {"subprocess.check_output", "subprocess.check_output"},
            {"subprocess.check_call",   "subprocess.check_call"},
            {"pickle.load",             "pickle.load"},
            {"pickle.loads",            "pickle.loads"},
            {"ctypes.CDLL",             "ctypes.CDLL"},
            {"ctypes.cdll",             "ctypes.cdll"},
            {"yaml.load",               "yaml.load"},
            {"importlib.import_module",  "importlib.import_module"},
        };

        // 2a: "from module import func as alias"
        try {
            std::regex re(
                "\\bfrom\\s+(\\w+(?:\\.\\w+)*)\\s+import\\s+(\\w+)\\s+as\\s+(\\w+)",
                std::regex::ECMAScript);
            auto begin = std::sregex_iterator(code.begin(), code.end(), re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                std::string full = (*m)[1].str() + "." + (*m)[2].str();
                auto it = IMPORT_MAP.find(full);
                if (it != IMPORT_MAP.end()) {
                    aliases[(*m)[3].str()] = it->second;
                }
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }

        // 2b: "from module import name1, name2, ..." (multi-import with optional "as")
        try {
            std::regex from_re(
                "\\bfrom\\s+(\\w+(?:\\.\\w+)*)\\s+import\\s+(.+)",
                std::regex::ECMAScript);
            auto begin = std::sregex_iterator(code.begin(), code.end(), from_re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                std::string mod = (*m)[1].str();
                std::string imports_str = (*m)[2].str();

                // Trim at newline/semicolon (stop at statement boundary)
                auto nl = imports_str.find_first_of("\n;");
                if (nl != std::string::npos) imports_str = imports_str.substr(0, nl);

                // Split by comma and process each token
                std::istringstream ss(imports_str);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    // Trim whitespace
                    size_t s = token.find_first_not_of(" \t");
                    size_t e = token.find_last_not_of(" \t");
                    if (s == std::string::npos) continue;
                    token = token.substr(s, e - s + 1);

                    // Check for "name as alias"
                    std::string func_name, alias_name;
                    auto as_pos = token.find(" as ");
                    if (as_pos != std::string::npos) {
                        func_name = token.substr(0, as_pos);
                        alias_name = token.substr(as_pos + 4);
                        // Trim both
                        func_name.erase(0, func_name.find_first_not_of(" \t"));
                        func_name.erase(func_name.find_last_not_of(" \t") + 1);
                        alias_name.erase(0, alias_name.find_first_not_of(" \t"));
                        alias_name.erase(alias_name.find_last_not_of(" \t") + 1);
                    } else {
                        func_name = token;
                        alias_name = token;
                    }

                    if (func_name == "*") continue;  // star handled separately

                    std::string full = mod + "." + func_name;
                    auto it = IMPORT_MAP.find(full);
                    if (it != IMPORT_MAP.end()) {
                        aliases[alias_name] = it->second;
                    }
                }
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }

        // 2c: "from module import *" — expand known dangerous functions from module
        try {
            std::regex star_re("\\bfrom\\s+(\\w+(?:\\.\\w+)*)\\s+import\\s+\\*",
                               std::regex::ECMAScript);
            auto begin = std::sregex_iterator(code.begin(), code.end(), star_re);
            auto end_it = std::sregex_iterator();
            for (auto m = begin; m != end_it; ++m) {
                std::string mod = (*m)[1].str();
                auto mod_it = DANGEROUS_MODULE_FUNCS.find(mod);
                if (mod_it != DANGEROUS_MODULE_FUNCS.end()) {
                    for (const auto& func : mod_it->second) {
                        aliases[func] = mod + "." + func;
                    }
                }
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    // ── Phase 3: Transitive alias chaining ──────────────────────────────
    // If b = a and a is already an alias, then b → canonical(a)
    for (int iter = 0; iter < 3; ++iter) {
        std::unordered_map<std::string, std::string> new_aliases;
        for (const auto& kv : aliases) {
            std::string pat = "(\\w+)\\s*" + assign_op + "\\s*\\b" +
                              kv.first + "\\b(?!\\s*\\()";
            try {
                std::regex re(pat, std::regex::ECMAScript);
                auto begin = std::sregex_iterator(working.begin(), working.end(), re);
                auto end_it = std::sregex_iterator();
                for (auto m = begin; m != end_it; ++m) {
                    auto pos = static_cast<size_t>((*m).position(1));
                    if (pos > 0 && working[pos - 1] == '.') continue;
                    std::string new_alias = (*m)[1].str();
                    if (new_alias != kv.first &&
                        aliases.find(new_alias) == aliases.end() &&
                        new_aliases.find(new_alias) == new_aliases.end()) {
                        new_aliases[new_alias] = kv.second;
                    }
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }
        if (new_aliases.empty()) break;
        for (const auto& kv : new_aliases) {
            aliases[kv.first] = kv.second;
        }
    }

    if (aliases.empty() && dict_aliases.empty()) return working;

    // ── Phase 4: Expand aliases in code ─────────────────────────────────

    // 4a: Function aliases — replace alias( with canonical(
    for (const auto& kv : aliases) {
        try {
            std::string call_pat = "\\b" + kv.first + "(\\s*\\()";
            std::regex call_re(call_pat, std::regex::ECMAScript);
            working = std::regex_replace(working, call_re, kv.second + "$1");
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    // 4b: Dict/list indirection — replace container[...](  with canonical(
    for (const auto& kv : dict_aliases) {
        try {
            std::string pat = "\\b" + kv.first + "\\s*\\[[^\\]]*\\](\\s*\\()";
            std::regex re(pat, std::regex::ECMAScript);
            working = std::regex_replace(working, re, kv.second + "$1");
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    return working;
}

// --- PII Detection ---
static const std::vector<std::pair<std::string, std::string>> DEFAULT_PII_PATTERNS = {
    {"\\b\\d{3}-\\d{2}-\\d{4}\\b", "SSN"},
    {"\\b\\d{4}[-\\s]?\\d{4}[-\\s]?\\d{4}[-\\s]?\\d{4}\\b", "Credit Card"},
    {"\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}\\b", "Email"},
    {"\\b(?:\\+?1[-.]?)?\\d{3}[-.]?\\d{3}[-.]?\\d{4}\\b", "Phone"},
    {"\\b(?:(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\.){3}(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\b", "IP Address"},
};

std::string GovernanceEngine::checkPii(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_pii;
    if (!cfg.enabled) return "";
    clearTrace();

    // DEFAULT_PII_PATTERNS is constant — compile once (runs on every agent
    // prompt/response/tool argument, so per-call compilation is hot).
    static const std::vector<std::regex> compiled_pii_patterns = [] {
        std::vector<std::regex> out;
        for (const auto& [pat, desc] : DEFAULT_PII_PATTERNS) {
            (void)desc;
            out.emplace_back(pat);
        }
        return out;
    }();

    std::vector<size_t> pattern_indices;
    if (cfg.detect_ssn) pattern_indices.push_back(0);
    if (cfg.detect_credit_card) pattern_indices.push_back(1);
    if (cfg.detect_email) pattern_indices.push_back(2);
    if (cfg.detect_phone) pattern_indices.push_back(3);
    if (cfg.detect_ip_address) pattern_indices.push_back(4);

    for (size_t pi : pattern_indices) {
        const auto& desc = DEFAULT_PII_PATTERNS[pi].second;
        try {
            const std::regex& re = compiled_pii_patterns[pi];
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string found = match[0].str();

                // Skip all-same-digit sequences (not real credit cards)
                // e.g., "0000000000000000" used as hash placeholders
                if (desc == "Credit Card") {
                    std::string digits_only;
                    for (char c : found) { if (std::isdigit(c)) digits_only += c; }
                    if (!digits_only.empty()) {
                        bool all_same = true;
                        for (size_t k = 1; k < digits_only.size(); ++k) {
                            if (digits_only[k] != digits_only[0]) { all_same = false; break; }
                        }
                        if (all_same) continue;
                    }
                }

                // Check allowlist
                bool allowed = false;
                for (const auto& a : cfg.allowlist_patterns) {
                    if (found.find(a) != std::string::npos) { allowed = true; break; }
                }
                if (allowed) continue;

                std::string display = cfg.mask_in_errors
                    ? (found.substr(0, 3) + std::string(found.size() - 3, '*'))
                    : found;
                addTrace(fmt::format("PII detected: {} at line {}", desc, line));
                return enforce("code_quality.no_pii", cfg.level,
                    formatError(cfg.level,
                        fmt::format("PII detected: {} ({})", desc, display),
                        line > 0 ? fmt::format("line {}", line) : "",
                        "code_quality.no_pii",
                        "Remove personally identifiable information from code.\n"
                        "Use environment variables or config files instead.",
                        "email = \"john.doe@company.com\"",
                        "email = env.get(\"ADMIN_EMAIL\")"));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        } catch (const std::bad_alloc&) {
            throw std::runtime_error(
                "Governance: memory exhausted during PII check — execution halted.\n"
                "  This is a fail-safe: governance cannot verify safety under memory pressure.\n"
            );
        }
    }
    recordPass("code_quality.no_pii", cfg.level);
    return "";
}

// --- Temporary Code ---
static const std::vector<std::string> DEFAULT_TEMP_PATTERNS = {
    // EVA-5: Comment-prefix-agnostic patterns — match # (Python/Shell/Ruby)
    // and // (JS/Go/Rust/NAAb/C++/C#/Java) comment styles
    "(?:#|//)\\s*[Ff]or now", "(?:#|//)\\s*[Tt]emporary", "(?:#|//)\\s*[Qq]uick fix",
    "(?:#|//)\\s*[Ww]ill implement later", "(?:#|//)\\s*[Ss]implified",
    "(?:#|//)\\s*[Bb]asic implementation", "(?:#|//)\\s*[Mm]inimal implementation",
    "(?:#|//)\\s*[Ww]ill (?:replace|refactor|rewrite)",
    "(?:#|//)\\s*[Nn]eeds? (?:refactoring|improvement|work)",
    "(?:#|//)\\s*[Ss]kipping for now", "(?:#|//)\\s*[Dd]efer(?:red)?",
    "(?:#|//)\\s*[Pp]rototype", "(?:#|//)\\s*[Ww]orkaround",
    "(?:#|//)\\s*[Bb]and.?aid",
    // Multi-line comment markers (C-style /* */)
    "\\*\\s*[Tt]emporary", "\\*\\s*[Ss]implified",
    "\\*\\s*[Ff]or now", "\\*\\s*[Pp]laceholder",
};

std::string GovernanceEngine::checkTemporaryCode(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_temporary_code;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_TEMP_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats, !cfg.case_sensitive);
    if (!found.empty()) {
        addTrace(fmt::format("matched temporary code pattern: \"{}\"", found));
        return enforce("code_quality.no_temporary_code", cfg.level,
            formatError(cfg.level, fmt::format("Temporary code marker: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_temporary_code",
                "The most common fix is to simply DELETE the comment.\n"
                "If the code underneath works, the comment was the only problem.\n"
                "Replace temporary code markers with production implementation.",
                "# for now, just return empty\nresult = []\nreturn result",
                "result = [analyze(item) for item in data]\nreturn result"));
    }
    recordPass("code_quality.no_temporary_code", cfg.level);
    return "";
}

// --- Simulation Markers ---
static const std::vector<std::string> DEFAULT_SIMULATION_PATTERNS = {
    "[Ss]imulate[ds]?", "[Mm]ock(?:ed|ing)?\\s+(?:execution|data|response|result)",
    "[Ww]ould\\s+(?:\\w+\\s+)?in\\s+production", "[Rr]eplace\\s+this\\s+with",
    "[Ff]ake\\s+(?:data|response|result|output|implementation)",
    "[Dd]ummy\\s+(?:data|response|result|output|implementation)",
    "[Ss]tub(?:bed)?\\s+(?:out|implementation|response)",
};

std::string GovernanceEngine::checkSimulationMarkers(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_simulation_markers;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_SIMULATION_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats, !cfg.case_sensitive);
    if (!found.empty()) {
        addTrace(fmt::format("matched simulation pattern: \"{}\"", found));
        return enforce("code_quality.no_simulation_markers", cfg.level,
            formatError(cfg.level, fmt::format("Simulation marker: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_simulation_markers",
                "Replace simulated/mocked code with real computation from actual data.",
                "# simulated result\nresult = random.uniform(0.8, 0.95)",
                "result = len(matches) / len(total) if total else 0.0"));
    }
    recordPass("code_quality.no_simulation_markers", cfg.level);
    return "";
}

// --- Mock Data ---
std::string GovernanceEngine::checkMockData(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_mock_data;
    if (!cfg.enabled) return "";
    clearTrace();

    auto& prefixes = cfg.variable_prefixes.empty()
        ? (const std::vector<std::string>&)(std::vector<std::string>{"mock_", "fake_", "dummy_", "stub_", "sample_", "example_"})
        : cfg.variable_prefixes;

    for (const auto& prefix : prefixes) {
        try {
            std::regex re("\\b" + prefix + "\\w+", std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                return enforce("code_quality.no_mock_data", cfg.level,
                    formatError(cfg.level, fmt::format("Mock data variable: \"{}\"", match[0].str()),
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_mock_data",
                        "Use real data sources instead of mock/fake data.\n"
                        "Rename variables to describe their actual purpose.",
                        "mock_users = [{\"name\": \"John\"}]",
                        "users = load_users(data_path)"));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    // Check literal patterns
    static const std::vector<std::string> default_literals = {
        "['\"]foo['\"]", "['\"]bar['\"]", "['\"]baz['\"]",
        "['\"]lorem ipsum['\"]", "['\"]John Doe['\"]", "['\"]Jane Doe['\"]",
        "['\"]123 Main St['\"]", "['\"]test@test\\.com['\"]",
    };
    auto& lits = cfg.literal_patterns.empty() ? default_literals : cfg.literal_patterns;
    std::string found = searchPatterns(code, lits);
    if (!found.empty()) {
        return enforce("code_quality.no_mock_data", cfg.level,
            formatError(cfg.level, fmt::format("Mock literal: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_mock_data",
                "Replace placeholder literals with computed or configured values.",
                "email = \"test@test.com\"\nname = \"John Doe\"",
                "email = config.get(\"admin_email\")\nname = user.get(\"display_name\")"));
    }

    recordPass("code_quality.no_mock_data", cfg.level);
    return "";
}

// --- Apologetic Language ---
std::string GovernanceEngine::checkApologeticLanguage(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_apologetic_language;
    if (!cfg.enabled) return "";
    clearTrace();

    static const std::vector<std::string> default_apology_patterns = {
        "[Ii]'?m\\s+(?:very\\s+)?sorry", "[Ii]\\s+apologize", "[Mm]y\\s+apologies",
        "[Oo]ops!?", "[Yy]ikes!?", "[Uu]h\\s+oh!?",
        "[Ii]'?ll\\s+fix\\s+(?:it|this)\\s+(?:immediately|right\\s+away)",
        "[Ii]\\s+didn'?t\\s+(?:check|verify|test)",
        "[Ii]\\s+should\\s+have\\s+(?:checked|verified|tested)",
    };

    std::string found = searchPatterns(code, default_apology_patterns);
    if (!found.empty()) {
        return enforce("code_quality.no_apologetic_language", cfg.level,
            formatError(cfg.level, fmt::format("Apologetic language: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_apologetic_language",
                "LLM-generated code should not contain apologies or self-deprecation.\n"
                "This indicates the code may not have been properly verified.\n"
                "Delete the apologetic comment — if the code works, ship it.",
                "# I'm sorry, this is a basic implementation\ndef process(data): pass",
                "def process(data):\n    return [transform(item) for item in data]"));
    }
    recordPass("code_quality.no_apologetic_language", cfg.level);
    return "";
}

// --- Dead Code ---
std::string GovernanceEngine::checkDeadCode(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_dead_code;
    if (!cfg.enabled) return "";
    clearTrace();

    static const std::vector<std::string> default_dead_patterns = {
        "if\\s+(?:True|1)\\s*:", "if\\s+(?:False|0)\\s*:",
        "except:\\s*(?:pass|\\.\\.\\.)\\s*$",
        // NAAb commented-out code patterns (Gate 18: commented-out logic detection)
        "//\\s*\\w+\\s*=\\s*(?:true|false|null|\\d|\")",   // // var = true/false/value (commented assignment)
        "//\\s*\\w+\\.push\\(",                              // // arr.push(x) (commented mutation)
        "//\\s*return\\s+\\w",                               // // return value (commented return)
    };

    auto& pats = cfg.patterns.empty() ? default_dead_patterns : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_dead_code", cfg.level,
            formatError(cfg.level, fmt::format("Dead code pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_dead_code",
                "Remove dead/unreachable code. Code behind always-true/false conditions\n"
                "or empty except blocks serves no purpose.",
                "if True:\n    do_thing()  # always runs — remove the if\nexcept: pass  # swallows errors",
                "do_thing()  # just call it directly\nexcept ValueError as e:\n    log.error(e); raise"));
    }
    recordPass("code_quality.no_dead_code", cfg.level);
    return "";
}

// --- Debug Artifacts ---
static const std::vector<std::string> DEFAULT_DEBUG_PATTERNS = {
    // Only flag print/log when clearly debug-related (contains "debug" keyword)
    "print\\(.*debug", "console\\.debug\\(",
    // Actual debug tools — these ARE debug artifacts
    "import\\s+pdb", "import\\s+ipdb", "breakpoint\\(\\)",
    "debugger;?", "binding\\.pry",
    // NOTE: fmt.Println, console.log, System.out.println are standard I/O,
    // NOT debug artifacts. They are the normal output mechanism in polyglot blocks.
};

std::string GovernanceEngine::checkDebugArtifacts(const std::string& language,
                                                   const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_debug_artifacts;
    if (!cfg.enabled) return "";
    clearTrace();

    auto& pats = cfg.patterns.empty() ? DEFAULT_DEBUG_PATTERNS : cfg.patterns;
    for (const auto& pat : pats) {
        try {
            std::regex re(pat, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string found = match[0].str();
                // Check allowlist
                bool allowed = false;
                for (const auto& a : cfg.allowlist) {
                    if (found.find(a) != std::string::npos) { allowed = true; break; }
                }
                if (allowed) continue;

                return enforce("code_quality.no_debug_artifacts", cfg.level,
                    formatError(cfg.level, fmt::format("Debug artifact in {} block: \"{}\"", language, found),
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_debug_artifacts",
                        "Remove debug statements before deployment.\n"
                        "Note: print(), console.log(), fmt.Println() are standard I/O, NOT debug.\n"
                        "Only debugger tools (pdb, breakpoint(), debugger;) are flagged.",
                        "import pdb; pdb.set_trace()\nbreakpoint()",
                        "# Remove debug tools entirely — use print() for normal output"));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }
    recordPass("code_quality.no_debug_artifacts", cfg.level);
    return "";
}

// --- Unsafe Deserialization ---
static const std::vector<std::string> DEFAULT_DESER_PATTERNS = {
    "pickle\\.loads?\\(", "yaml\\.load\\(", "yaml\\.unsafe_load",
    "marshal\\.load", "shelve\\.open", "jsonpickle\\.decode",
    "unserialize\\(", "ObjectInputStream", "BinaryFormatter\\.Deserialize",
};

std::string GovernanceEngine::checkUnsafeDeserialization(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_unsafe_deserialization;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_DESER_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_unsafe_deserialization", cfg.level,
            formatError(cfg.level, fmt::format("Unsafe deserialization: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_unsafe_deserialization",
                "Use safe deserialization methods (json.loads, yaml.safe_load).\n"
                "Unsafe deserializers can execute arbitrary code from untrusted input.",
                "data = pickle.loads(user_input)\nconfig = yaml.load(file)",
                "data = json.loads(user_input)\nconfig = yaml.safe_load(file)"));
    }
    recordPass("code_quality.no_unsafe_deserialization", cfg.level);
    return "";
}

// --- SQL Injection ---
static const std::vector<std::string> DEFAULT_SQL_PATTERNS = {
    "(?:SELECT|INSERT|UPDATE|DELETE|DROP|ALTER)\\s+.*['\"]\\s*\\+",
    "(?:SELECT|INSERT|UPDATE|DELETE)\\s+.*%s",
    "f['\"].*(?:SELECT|INSERT|UPDATE|DELETE).*\\{",
    "\\.format\\(.*(?:SELECT|INSERT|UPDATE|DELETE)",
};

std::string GovernanceEngine::checkSqlInjection(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_sql_injection;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_SQL_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_sql_injection", cfg.level,
            formatError(cfg.level, "SQL injection pattern detected",
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_sql_injection",
                "Use parameterized queries instead of string concatenation",
                "cursor.execute(\"SELECT * FROM users WHERE id=\" + user_id)",
                "cursor.execute(\"SELECT * FROM users WHERE id=?\", (user_id,))"));
    }
    recordPass("code_quality.no_sql_injection", cfg.level);
    return "";
}

// --- Path Traversal ---
static const std::vector<std::string> DEFAULT_PATH_PATTERNS = {
    // FIX 23: Require path context to avoid ellipsis false positives
    // "Initializing... /tmp" no longer triggers (g before .., 3 dots)
    "(?:^|[/\\\\\"'\\s])\\.\\.(?:/|\\\\)",  // ../ or ..\ preceded by path separator, quote, or space
    "%2e%2e(?:%2f|/)",                       // URL-encoded traversal
    "\\.\\.%2f",                             // Partial URL-encoding
};

std::string GovernanceEngine::checkPathTraversal(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_path_traversal;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_PATH_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        // FIX 29: Include matched text in error for easier debugging
        return enforce("code_quality.no_path_traversal", cfg.level,
            formatError(cfg.level,
                fmt::format("Path traversal pattern detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_path_traversal",
                "Use absolute paths or os.path.realpath() to prevent traversal.\n"
                "Never construct paths by concatenating user input with '..'.",
                "path = base_dir + \"/../\" + user_input",
                "path = os.path.realpath(os.path.join(base_dir, user_input))\n"
                "assert path.startswith(base_dir)  # validate resolved path"));
    }
    recordPass("code_quality.no_path_traversal", cfg.level);
    return "";
}

// --- Hardcoded URLs ---
std::string GovernanceEngine::checkHardcodedUrls(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_hardcoded_urls;
    if (!cfg.enabled) return "";
    clearTrace();
    try {
        std::regex re("https?://(?!example\\.com|localhost|127\\.0\\.0\\.1|0\\.0\\.0\\.0)[a-zA-Z0-9.-]+");
        std::smatch match;
        if (std::regex_search(code, match, re)) {
            std::string url = match[0].str();
            for (const auto& a : cfg.allowlist) {
                if (url.find(a) != std::string::npos) { recordPass("code_quality.no_hardcoded_urls", cfg.level); return ""; }
            }
            return enforce("code_quality.no_hardcoded_urls", cfg.level,
                formatError(cfg.level, fmt::format("Hardcoded URL: \"{}\"", url),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_hardcoded_urls",
                    "Use configuration or environment variables for URLs.\n"
                    "localhost, 127.0.0.1, and example.com are allowed.",
                    "api_url = \"https://api.production.com/v1\"",
                    "api_url = os.environ.get(\"API_URL\", \"http://localhost:8080\")"));
        }
    } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    recordPass("code_quality.no_hardcoded_urls", cfg.level);
    return "";
}

// --- Hardcoded IPs ---
std::string GovernanceEngine::checkHardcodedIps(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_hardcoded_ips;
    if (!cfg.enabled) return "";
    clearTrace();
    try {
        std::regex re("\\b(?:(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\.){3}(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\b");
        std::smatch match;
        if (std::regex_search(code, match, re)) {
            std::string ip = match[0].str();
            static const std::vector<std::string> default_allow = {"127.0.0.1", "0.0.0.0", "255.255.255.0", "255.255.255.255"};
            auto& allowlist = cfg.allowlist.empty() ? default_allow : cfg.allowlist;
            for (const auto& a : allowlist) { if (ip == a) { recordPass("code_quality.no_hardcoded_ips", cfg.level); return ""; } }
            return enforce("code_quality.no_hardcoded_ips", cfg.level,
                formatError(cfg.level, fmt::format("Hardcoded IP: \"{}\"", ip),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_hardcoded_ips",
                    "Use configuration or DNS for IP addresses.\n"
                    "127.0.0.1, 0.0.0.0, and 255.x.x.x are allowed.",
                    "server = \"192.168.1.100\"",
                    "server = os.environ.get(\"SERVER_HOST\", \"localhost\")"));
        }
    } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    recordPass("code_quality.no_hardcoded_ips", cfg.level);
    return "";
}

// --- Encoding ---
std::string GovernanceEngine::checkEncoding(const std::string& code, int line) {
    auto& cfg = rules().code_quality.encoding;
    if (!cfg.enabled) return "";
    clearTrace();

    if (cfg.block_null_bytes && code.find('\0') != std::string::npos) {
        return enforce("code_quality.encoding", cfg.level,
            formatError(cfg.level, "Null byte detected in code",
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.encoding.block_null_bytes",
                "Null bytes can be used for injection attacks.\n"
                "Ensure code does not contain embedded \\0 characters.",
                "data = \"hello\\x00world\"  # hidden null byte",
                "data = \"helloworld\"  # clean string"));
    }
    if (cfg.block_unicode_bidi && code.size() >= 3) {
        // H8 fix: guard against size_t underflow when code.size() < 3
        // Check for Unicode bidirectional override characters
        for (size_t i = 0; i <= code.size() - 3; i++) {
            unsigned char c1 = code[i], c2 = code[i+1], c3 = code[i+2];
            // U+202A-U+202E (bidi overrides, UTF-8: E2 80 AA-AE)
            if (c1 == 0xE2 && c2 == 0x80 && (c3 >= 0xAA && c3 <= 0xAE)) {
                return enforce("code_quality.encoding", cfg.level,
                    formatError(cfg.level, "Unicode bidirectional override character detected",
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.encoding.block_unicode_bidi",
                        "Bidi override characters can be used for trojan source attacks.\n"
                        "These invisible Unicode characters make code appear different from what executes.",
                        "access_level = \"user\u202Enimda\"  # hidden bidi override",
                        "access_level = \"admin\"  # plain ASCII string"));
            }
            // U+2066-U+2069 (directional isolates, UTF-8: E2 81 A6-A9)
            if (c1 == 0xE2 && c2 == 0x81 && (c3 >= 0xA6 && c3 <= 0xA9)) {
                return enforce("code_quality.encoding", cfg.level,
                    formatError(cfg.level, "Unicode directional isolate character detected",
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.encoding.block_unicode_bidi",
                        "Directional isolate characters can be used for trojan source attacks.\n"
                        "These invisible Unicode characters make code appear different from what executes.",
                        "access_level = \"user\u2066nimda\"  # hidden directional isolate",
                        "access_level = \"admin\"  # plain ASCII string"));
            }
        }
    }
    recordPass("code_quality.encoding", cfg.level);
    return "";
}

// --- Complexity ---
std::string GovernanceEngine::checkComplexity(const std::string& code, int line) {
    auto& cfg = rules().code_quality.max_complexity;
    if (!cfg.enabled) return "";
    clearTrace();

    if (cfg.max_lines_per_block > 0) {
        int lines = 1;
        for (char c : code) if (c == '\n') lines++;
        if (lines > cfg.max_lines_per_block) {
            return enforce("code_quality.max_complexity", cfg.level,
                formatError(cfg.level, fmt::format("Block has {} lines (max: {})", lines, cfg.max_lines_per_block),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.max_complexity.max_lines_per_block",
                    "Break large blocks into smaller functions or multiple blocks.\n"
                    "Extract helpers and call them from the main block.",
                    "<<python\n# 200 lines of code in one block\n>>",
                    "<<python\ndef helper_a(): ...\ndef helper_b(): ...\nresult = helper_a() + helper_b()\n>>"));
        }
    }
    recordPass("code_quality.max_complexity", cfg.level);
    return "";
}

// ============================================================================
// LLM Anti-Drift Checks
// ============================================================================

// --- Oversimplification ---
static const std::vector<std::string> DEFAULT_OVERSIMPLIFICATION_PATTERNS = {
    // Empty/pass-only function bodies
    "def\\s+\\w+\\([^)]*\\):\\s*pass\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*\\.\\.\\.",
    "function\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    "\\w+\\s*=\\s*\\([^)]*\\)\\s*=>\\s*\\{\\s*\\}",
    "=>\\s*(?:null|undefined|None)\\s*[;\\n]",
    "lambda\\s+[^:]+:\\s*None",
    "func\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    // Trivial return stubs
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+True\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+False\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+None\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+0\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\"\"\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\\[\\]\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\\{\\}\\s*$",
    "function\\s+\\w+[^{]*\\{\\s*return\\s+(?:true|false|null|undefined|0|\"\"|''|\\[\\]|\\{\\})\\s*;?\\s*\\}",
    // Identity/passthrough functions
    "def\\s+validate\\w*\\([^)]+\\):\\s*return\\s+True",
    "def\\s+check\\w*\\([^)]+\\):\\s*return\\s+True",
    "def\\s+is_\\w+\\([^)]+\\):\\s*return\\s+True",
    // Not implemented markers
    "raise\\s+NotImplementedError",
    "throw\\s+new\\s+Error\\([\"']not\\s+implemented",
    "throw\\s+new\\s+Error\\([\"']TODO",
    "panic!\\([\"']not\\s+implemented",
    "panic!\\([\"']todo",
    "unimplemented!\\(\\)",
    "todo!\\(\\)",
    // Comment-only / placeholder bodies
    "#\\s*implementation\\s+here",
    "//\\s*implementation\\s+here",
    "/\\*\\s*\\.\\.\\.\\s*\\*/",
    "#\\s*your\\s+code\\s+here",
    "//\\s*TODO:?\\s*implement",
    "#\\s*add\\s+(?:your|actual|real)\\s+(?:code|logic|implementation)",
    // Hardcoded/fabricated results
    "return\\s+\\{[\"']status[\"']:\\s*[\"'](?:ok|success|done)[\"']",
    "print\\([\"'](?:Processing|Done|Complete|Success|Working)[\"']\\)\\s*$",

    // ============================================================
    // LLM stub evasion patterns — comments that admit code is incomplete
    // EVA-5: Merged #//-prefixed patterns into (?:#|//) for all comment styles
    // ============================================================

    // ============================================================
    // EVA-5+EXTRA-1 UNIFIED: Co-occurrence stub detection
    // Instead of matching exact phrases ("basic implementation"),
    // detect ANY comment containing both a diminishing adjective
    // and a code noun, in any word order.
    // ============================================================

    // Diminishing adjective + code noun (any order)
    "(?:#|//)(?=.*\\b(?:basic|simplified|simple|minimal|trivial|naive|rough|crude|approximate|toy|pedagogical|illustrative|proof.of.concept|MVP|bare.minimum|stripped.down|condensed|abridged|truncated)\\b)(?=.*\\b(?:implementation|version|approach|logic|solution|code|algorithm|example)\\b).*",

    // Mock/fake/placeholder + data/result/value (any order)
    "(?:#|//)(?=.*\\b(?:mock|dummy|fake|placeholder|simulated|hardcoded|synthetic|fabricated|generated|contrived|artificial|stand.?in|filler|surrogate|substitute)\\b)(?=.*\\b(?:data|implementation|results?|value|response|output|return|computation|calculation)\\b).*",

    // "will/should be" + "replaced/implemented/completed" (any order)
    "(?:#|//)(?=.*\\b(?:will|should|needs?|could|can|to)\\b)(?=.*\\bbe\\b)(?=.*\\b(?:replaced|implemented|completed|finished|done|updated|improved|expanded|fleshed)\\b).*",

    // "left/serves/acts as" + "exercise/placeholder/skeleton" (any order)
    "(?:#|//)(?=.*\\b(?:left|serves?|acts?|used)\\b)(?=.*\\bas\\b)(?=.*\\b(?:exercise|example|placeholder|starting.point|skeleton|template|scaffold|stub|fallback)\\b).*",

    // "in a real/production" + "system/implementation" (any order)
    "(?:#|//)(?=.*\\b(?:real|production|actual|proper|full|complete)\\b)(?=.*\\b(?:system|application|implementation|version|world|scenario|environment)\\b)(?=.*\\b(?:in|for|would|should|goes|belongs)\\b).*",

    // "not fully implemented" — 3 groups to avoid false positives
    "(?:#|//)(?=.*\\b(?:not|partially|incompletely|never)\\b)(?=.*\\b(?:fully|completely|properly|actually|really|truly)\\b)(?=.*\\b(?:implemented|finished|done|functional|working|complete|developed)\\b).*",
    // "barely/hardly" + state (2 groups — these words imply both negation and degree)
    "(?:#|//)(?=.*\\b(?:barely|hardly)\\b)(?=.*\\b(?:implemented|finished|done|functional|working|complete|developed)\\b).*",

    // "omitted/elided for brevity/simplicity" (any order)
    "(?:#|//)(?=.*\\b(?:omitted|elided|redacted|removed|cut|truncated)\\b)(?=.*\\b(?:brevity|space|time|simplicity|clarity|readability)\\b).*",

    // Scaffold/skeleton + code noun (any order)
    "(?:#|//)(?=.*\\b(?:skeleton|boilerplate|starter|template|scaffold)\\b)(?=.*\\b(?:code|implementation|version)\\b).*",

    // --- Patterns that work fine as phrase-ordered (unique structures) ---
    "(?:#|//)\\s*(?:for|in)\\s+(?:demonstration|demo|testing|test|example|simplicity|now|this exercise)\\s*(?:purposes?|only)?",
    "(?:#|//)\\s*(?:this is|this was|here we|we just|i just|just)\\s+(?:a simplified|just a|only a|a basic|a rough|a naive|a quick|a temporary|a stopgap|using a simple)",
    "(?:#|//)\\s*(?:for now|temporary|interim|stopgap|quick and dirty|quick fix|short.?term|band.?aid)",
    "(?:#|//).*\\bsimplified\\b.*\\bfor now\\b",
    "(?:#|//)\\s*(?:no.op|noop|no operation|does nothing|empty|stub|pass.?through|identity|dummy|skip|bypass|shortcut)",
    "(?:#|//)\\s*(?:would normally|would actually|should actually|in reality|ideally|in practice)\\s+(?:do|use|call|implement|process|compute|calculate|analyze|check|validate)",
    "(?:#|//)\\s*(?:skipping|omitting|ignoring|bypassing|not doing|not implementing|eliding)\\s+(?:actual|real|proper|full|the)\\s+(?:logic|implementation|computation|analysis|processing|validation|checking)",
    "(?:#|//)\\s*(?:for brevity|for simplicity|for clarity|for readability)",

    // ============================================================
    // EVA-9: NAAb-specific function stub patterns (fn keyword, {} braces)
    // ============================================================
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+true\\s*\\}",       // fn x() { return true }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+false\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+null\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+0\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\\[\\]\\s*\\}",     // fn x() { return [] }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\\{\\}\\s*\\}",     // fn x() { return {} }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\"\"\\s*\\}",       // fn x() { return "" }

    // ============================================================
    // EVA-EXTRA-2: Complexity gaming / scanner manipulation
    // Uses CO-OCCURRENCE detection: any comment containing both a
    // governance concept (Group A) and a gaming intent word (Group B)
    // in ANY order gets flagged. This eliminates whack-a-mole with
    // specific phrase orderings ("complexity padding" vs "padding
    // for complexity" vs "to boost complexity" etc.)
    // ============================================================

    // Co-occurrence: governance concept + gaming intent in same comment (any order)
    // Group A: complexity|score|threshold|floor|scanner|checker|validator|gate|requirement
    // Group B: padding|gaming|hack|boost|inflate|trick|fool|filler|workaround|fake|dummy|artificial
    // Note: "satisfy" excluded from Group B (too common in normal comments like
    // "satisfy the user requirement") — handled separately with scanner-specific Group A
    "(?:#|//)(?=.*\\b(?:complexity|score|threshold|floor|scanner|checker|validator|gate|requirement)\\b)(?=.*\\b(?:padding|gaming|hack|boost|inflate|trick|fool|filler|workaround|fake|dummy|artificial)\\b).*",
    // "satisfy" only flags with scanner-specific concepts (not generic "requirement")
    "(?:#|//)(?=.*\\b(?:complexity|score|threshold|floor|scanner|checker|validator|gate)\\b)(?=.*\\b(?:satisfy)\\b).*",

    // Co-occurrence variant: "just/only [doing X] to/for [governance concept]"
    "(?:#|//)(?=.*\\b(?:just|only|merely|simply)\\b)(?=.*\\b(?:to|for)\\b)(?=.*\\b(?:complexity|score|threshold|floor|scanner|checker|validator|gate|requirement|pass|meet|reach|satisfy)\\b).*",

    // Score calculation/estimate comments (admitting score tracking)
    "(?:#|//).*score\\s*(?:estimate|calculation|breakdown|:).*(?:need|require|missing|more|short)",
    // Score threshold comments: "Complexity score >= 20 required"
    "(?:#|//).*(?:complexity|score).*(?:>=?|>|==|is)\\s*\\d+\\s*(?:required|needed|necessary)",

    // Purpose-admission: comments that admit code serves no real purpose
    "(?:#|//)\\s*(?:mandatory|required|needed|necessary)\\s+(?:complexity|padding|score|loop|iteration)",
    "(?:#|//).*\\b(?:no-op|noop|dead code|unused|pointless|useless)\\b.*\\b(?:complexity|score|floor|gate|pass)\\b",
};

std::string GovernanceEngine::checkOversimplification(const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_oversimplification;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_OVERSIMPLIFICATION_PATTERNS : cfg.patterns;

    // Build active patterns based on config flags
    std::vector<std::string> active_pats;
    for (const auto& p : pats) active_pats.push_back(p);
    for (const auto& p : cfg.custom_patterns) active_pats.push_back(p);
    addTrace(fmt::format("checking {} patterns ({} built-in + {} custom)",
        active_pats.size(), pats.size(), cfg.custom_patterns.size()));

    std::string found = searchPatterns(code, active_pats, !cfg.case_sensitive);
    if (!found.empty()) {
        addTrace(fmt::format("matched pattern: \"{}\"", found));
        return enforce("code_quality.no_oversimplification", cfg.level,
            formatError(cfg.level, fmt::format("Oversimplified code detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_oversimplification",
                "This code is a stub, placeholder, or trivially incomplete.\n"
                "The most common fix: DELETE the qualifying comment.\n"
                "If the code underneath works, the comment was the only problem.\n"
                "Comments should explain WHY, not admit the code is incomplete.\n\n"
                "  Do NOT add hedging comments like:\n"
                "    \"for now\", \"simplified\", \"basic implementation\",\n"
                "    \"in a real system\", \"would normally\", \"for demonstration\"",
                "// basic implementation for now\n"
                "fn process(data) {\n"
                "    return data  // would normally transform\n"
                "}",
                "fn process(data) {\n"
                "    let result = []\n"
                "    for item in data {\n"
                "        let transformed = apply_rules(item)\n"
                "        result.push(transformed)\n"
                "    }\n"
                "    return result\n"
                "}"));
    }
    recordPass("code_quality.no_oversimplification", cfg.level);
    return "";
}

// --- Incomplete Logic ---
static const std::vector<std::string> DEFAULT_INCOMPLETE_LOGIC_PATTERNS = {
    // Empty/swallowed error handling
    "except:\\s*pass",
    "except\\s+\\w+(?:\\s+as\\s+\\w+)?:\\s*pass",
    "except\\s+\\w+(?:\\s+as\\s+\\w+)?:\\s*\\.\\.\\.",
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}",
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*//",
    "except[^:]*:\\s*print\\([\"'](?:error|Error|ERROR)[\"']\\)",
    "catch\\s*\\(\\w+\\)\\s*\\{\\s*console\\.log\\(\\w+\\)\\s*;?\\s*\\}",
    "rescue\\s*(?:=>)?\\s*(?:nil|end)",
    "except[^:]*:\\s*return\\s+None",
    "except[^:]*:\\s*return\\s+(?:False|\\[\\]|\\{\\}|\"\"|0)",
    // Bare/generic error raising
    "raise\\s+Exception\\s*$",
    "raise\\s+Exception\\([\"'](?:error|Error|e|err|exception)[\"']\\)",
    "raise\\s+Exception\\([\"'](?:something went wrong|an error occurred|failed|unknown error)[\"']\\)",
    "raise\\s+Exception\\([\"'](?:todo|fixme|implement|not done)[\"']\\)",
    "throw\\s+new\\s+Error\\([\"'](?:error|Error|e|err)[\"']\\)",
    "throw\\s+new\\s+Error\\([\"'](?:something went wrong|failed|unknown)[\"']\\)",
    "raise\\s+ValueError\\([\"'](?:invalid|bad|wrong)\\s*(?:value|input|data)?[\"']\\)",
    // Degenerate loops
    "for\\s+\\w+\\s+in\\s+\\w+:\\s*return\\s+\\w+",
    "for\\s+\\w+\\s+in\\s+\\w+:\\s*break",
    "for\\s*\\([^)]*\\)\\s*\\{\\s*return",
    "while\\s+True:\\s*break",
    "for\\s+\\w+\\s+in\\s+range\\(1\\)",
    // Dummy/incomplete conditionals
    "if\\s+True\\s*:",
    "if\\s+False\\s*:",
    "if\\s*\\(\\s*true\\s*\\)",
    "if\\s*\\(\\s*false\\s*\\)",
    "if\\s+1\\s*:",
    "if\\s+0\\s*:",
    "if\\s+.*:\\s*pass\\s*$",
    "else:\\s*pass\\s*$",
    // Placeholder error messages
    "[\"'](?:Something went wrong|An error occurred|Failed|Unknown error|Unexpected error)[\"']",

    // ============================================================
    // LLM no-op / passthrough disguises
    // ============================================================

    // Return empty collections (suspicious when in analysis/compute functions)
    "return\\s+\\[\\]\\s*(?:#|//|$)",
    "return\\s+\\{\\}\\s*(?:#|//|$)",

    // Passthrough/identity functions
    "return\\s+(?:input|data|text|value|args?|params?|x|obj|item|content|payload|message|request|response)\\s*(?:#|//)\\s*(?:unchanged|as.?is|pass.?through|no.?change|unmodified|untouched|directly)",

    // Random/simulated data generators (pretending to compute real results)
    "random\\.(?:random|uniform|randint|choice|gauss|sample|shuffle)\\(\\)\\s*(?:#|//)",
    "Math\\.random\\(\\)\\s*(?:#|//)",

    // Hardcoded score arrays (suspicious in analysis functions)
    "scores?\\s*=\\s*\\[\\s*(?:0\\.\\d+,?\\s*){3,}\\]",

    // Hardcoded numeric results (suspicious in analysis functions)
    // Only flag suspiciously precise values (0.85, 0.92, etc.), NOT common initializers (0.0, 0.5, 1.0)
    "(?:score|accuracy|precision|recall|f1|confidence|probability|similarity|distance|weight)\\s*=\\s*0\\.[1-9]\\d{1,}\\s*(?:#|//|$)",

    // Degenerate implementations
    "for\\s+\\w+\\s+in\\s+\\w+\\s*\\{\\s*\\}",
    "while\\s+\\w+\\s*\\{\\s*break\\s*\\}",

    // EVA-9: NAAb-specific incomplete logic patterns
    // NAAb catch-and-swallow (empty catch blocks)
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}",
    // NAAb identity/passthrough validation functions
    "fn\\s+(?:validate|check|verify|is_)\\w*\\([^)]*\\)\\s*\\{\\s*return\\s+true\\s*\\}",
    // NAAb identity sanitizer: validate_*(param) { return param } — returns input unchanged
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*return\\s+\\1\\s*;?\\s*\\}",
    // NAAb identity pipeline: x |> fn(param) { return param } — adds complexity points for nothing
    "\\|>\\s*fn\\s*\\(\\s*\\w+\\s*\\)\\s*\\{\\s*return\\s+\\w+\\s*;?\\s*\\}",
    // NAAb error sentinel: validate_*/check_* returning hardcoded error string instead of throwing
    "fn\\s+(?:validate|check|verify)_\\w+\\([^)]*\\)\\s*\\{[^}]*return\\s+\"(?:invalid|error|failed|unknown|missing|bad|none)[^\"]*\"",
    // NAAb copy-and-return identity sanitizer: validate_*(p) { let x = p; return x }
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*let\\s+(\\w+)\\s*=\\s*\\1\\s*;?\\s*return\\s+\\2\\s*;?\\s*\\}",
    // NAAb multi-let copy-and-return: validate_*(p) { let a = p; let b = a; return b }
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*(?:let\\s+\\w+\\s*=\\s*\\w+\\s*;?\\s*){1,3}return\\s+\\w+\\s*;?\\s*\\}",

    // EVA-EXTRA-4: REMOVED — cosmetic sanitizer pattern matching replaced by
    // checkCosmeticSanitizer() which inverts the logic: checks for PRESENCE of
    // real sanitization ops rather than listing cosmetic transforms.
    // Identity sanitizer patterns (lines 892-902 above) kept as fast-path.

    // Effectless loop: single-line loop body with only a let declaration (no function calls)
    "for\\s+\\w+\\s+in\\s+[^{\\n]+\\{\\s*let\\s+\\w+\\s*=\\s*\\w+\\[\\w+\\]\\s*;?\\s*\\}",

    // EVA-EXTRA-3: Numeric result fabrication patterns
    // Suspiciously precise hardcoded scores (LLMs love 0.85, 0.92, etc.)
    "(?:score|accuracy|precision|recall|f1|confidence|similarity)\\s*=\\s*0\\.[89]\\d\\s*(?:#|//|$)",
    // Array of hardcoded floats (fabricated results)
    "\\[\\s*(?:0\\.\\d+,\\s*){4,}\\]",

    // EVA-EXTRA-5: Generic-fallback narrative strings
    // "X performed/completed/analyzed for phase/type/category: " + variable
    // Classic LLM stub: looks dynamic but contains no real logic
    "\"[A-Za-z]\\w*\\s+(?:performed|completed|done|executed|processed|analyzed|finished)\\s+"
    "(?:for|on|with)\\s+(?:phase|type|category|step|stage|mode|kind|state):\\s*\"\\s*\\+\\s*\\w+",

    // Hardcoded mid-range confidence without any supporting calculation
    // confidence = 0.3 / 0.4 / 0.5 — tells the caller "I'm unsure" without doing any work
    "\\bconfidence\\s*=\\s*0\\.[3-5](?:[^\\d.]|$)",
};

std::string GovernanceEngine::checkEmptyMain(const std::string& source) {
    auto& cfg = rules().code_quality.no_incomplete_logic;
    if (!cfg.enabled) return "";
    clearTrace();

    std::string main_body = extractMainBodyPublic(source);

    // Count comment vs code lines in main body
    int comment_lines = 0, code_lines = 0;
    bool in_block_comment = false;
    std::istringstream stream(main_body);
    std::string line_str;
    while (std::getline(stream, line_str)) {
        size_t first = line_str.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        std::string trimmed = line_str.substr(first);
        if (in_block_comment) {
            comment_lines++;
            if (trimmed.find("*/") != std::string::npos) in_block_comment = false;
            continue;
        }
        if (trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0) {
            comment_lines++;
        } else if (trimmed.rfind("/*", 0) == 0) {
            comment_lines++;
            if (trimmed.find("*/") == std::string::npos) in_block_comment = true;
        } else {
            code_lines++;
        }
    }

    if (code_lines == 0) {
        std::string detail = main_body.empty()
            ? "main{} block is empty"
            : "main{} block contains only comments (" + std::to_string(comment_lines) + " comment lines, 0 code lines)";
        return enforce("code_quality.no_incomplete_logic",
            EnforcementLevel::ADVISORY,
            detail + " — add executable code inside main{}.\n"
            "  Example:\n"
            "    main {\n"
            "      let result = compute_something()\n"
            "      print(result)\n"
            "    }\n"
            "  If this file is import-only (no entry point), remove main{}\n"
            "  or add the code that uses the exported functions.");
    }
    return "";
}

// --- Intent Validation: verify code matches declared intent ---
// Authority hierarchy:
//   1. Owner's function_intents in govern.json (ground truth, cfg.level)
//   2. Owner's project_intent in govern.json (broad context, advisory)
//   3. LLM's /// @intent (self-declared, advisory only — lower trust)

// Helper: extract meaningful keywords from freeform text
static std::vector<std::string> extractIntentKeywords(const std::string& text) {
    static const std::unordered_set<std::string> stop_words = {
        "a", "an", "the", "and", "or", "but", "in", "on", "at", "to", "for",
        "of", "with", "by", "from", "is", "are", "was", "were", "be", "been",
        "has", "have", "had", "do", "does", "did", "will", "would", "could",
        "should", "may", "might", "must", "shall", "can", "this", "that",
        "it", "its", "they", "them", "their", "we", "our", "you", "your",
        "if", "then", "else", "when", "where", "how", "what", "which",
        "each", "every", "all", "any", "both", "few", "more", "most",
        "using", "into", "not", "no", "see", "also", "return", "returns"
    };
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::vector<std::string> keywords;
    std::unordered_set<std::string> seen;
    std::string word;
    for (size_t i = 0; i <= lower.size(); i++) {
        char c = (i < lower.size()) ? lower[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            word += c;
        } else {
            if (word.size() >= 3 && stop_words.find(word) == stop_words.end()) {
                if (seen.insert(word).second) {
                    keywords.push_back(word);
                }
            }
            word.clear();
        }
    }
    return keywords;
}

// Helper: strip comments, string literals, and locally-declared variable names from code
// so they don't inflate keyword matches during intent validation.
// Two-pass: first collect variable names from `let` declarations, then remove all
// occurrences of those names plus comments and strings.
static std::string stripNonCodeContent(const std::string& code) {
    // Pass 1: collect variable names from `let <name>` declarations
    std::unordered_set<std::string> local_vars;
    std::unordered_set<std::string> let_string_values;  // strings from `let x = "..."` (stuffing vector)
    {
        size_t i = 0;
        while (i < code.size()) {
            // Skip strings
            if (code[i] == '"') {
                i++;
                while (i < code.size() && code[i] != '"') {
                    if (code[i] == '\\' && i + 1 < code.size()) i++;
                    i++;
                }
                if (i < code.size()) i++;
                continue;
            }
            // Find `let ` keyword
            if (i + 3 < code.size() && code.substr(i, 4) == "let ") {
                i += 4;
                while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) i++;
                std::string name;
                while (i < code.size() &&
                       (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) {
                    name += code[i]; i++;
                }
                if (name.size() >= 3) {
                    local_vars.insert(name);
                }
                // Check for = "string" pattern to detect let-assignment strings
                size_t j = i;
                while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) j++;
                if (j < code.size() && code[j] == '=') {
                    j++;
                    while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) j++;
                    if (j < code.size() && code[j] == '"') {
                        j++;
                        std::string str_val;
                        while (j < code.size() && code[j] != '"') {
                            if (code[j] == '\\' && j + 1 < code.size()) {
                                str_val += code[j]; str_val += code[j + 1];
                                j += 2;
                            } else {
                                str_val += code[j]; j++;
                            }
                        }
                        if (!str_val.empty() && str_val.size() <= 20) {
                            let_string_values.insert(str_val);
                        }
                    }
                }
                continue;
            }
            i++;
        }
    }

    // Collect function parameter names from declaration: fn name(p1, p2, p3) {
    {
        size_t fn_pos = code.find("fn ");
        if (fn_pos != std::string::npos) {
            size_t paren = code.find('(', fn_pos);
            size_t close = code.find(')', paren != std::string::npos ? paren : 0);
            if (paren != std::string::npos && close != std::string::npos) {
                std::string params = code.substr(paren + 1, close - paren - 1);
                std::string pname;
                for (size_t p = 0; p <= params.size(); p++) {
                    char c = (p < params.size()) ? params[p] : ',';
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                        pname += c;
                    } else {
                        if (pname.size() >= 3) local_vars.insert(pname);
                        pname.clear();
                    }
                }
            }
        }
    }

    // Collect for-loop variable names: for <name> in ...
    {
        size_t fi = 0;
        while (fi < code.size()) {
            if (fi + 3 < code.size() && code.substr(fi, 4) == "for ") {
                fi += 4;
                while (fi < code.size() && (code[fi] == ' ' || code[fi] == '\t')) fi++;
                std::string name;
                while (fi < code.size() &&
                       (std::isalnum(static_cast<unsigned char>(code[fi])) || code[fi] == '_')) {
                    name += code[fi]; fi++;
                }
                if (name.size() >= 3) local_vars.insert(name);
            }
            fi++;
        }
    }

    // Pass 2: strip comments, long strings (>20 chars, keyword stuffing), and local var names
    std::string result;
    result.reserve(code.size());
    size_t i = 0;
    while (i < code.size()) {
        // Skip /* */ block comments
        if (i + 1 < code.size() && code[i] == '/' && code[i + 1] == '*') {
            i += 2;
            while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/')) {
                i++;
            }
            if (i + 1 < code.size()) {
                i += 2;  // skip */
            } else {
                i = code.size();  // unterminated — skip to end
            }
            result += ' ';
            continue;
        }
        // Skip /// and // comments
        if (i + 1 < code.size() && code[i] == '/' && code[i + 1] == '/') {
            while (i < code.size() && code[i] != '\n') i++;
            continue;
        }
        // Handle string literals: keep short ones (dict keys, field names), strip long ones
        if (code[i] == '"') {
            i++;
            std::string str_content;
            while (i < code.size() && code[i] != '"') {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    str_content += code[i]; str_content += code[i + 1];
                    i += 2;
                } else {
                    str_content += code[i]; i++;
                }
            }
            if (i < code.size()) i++; // skip closing quote
            // Keep short strings (<= 20 chars) — likely dict keys, field names, format strings
            // Strip long strings — likely keyword stuffing / intent restatements
            // Strip let-assignment strings — `let x = "keyword"` is a stuffing vector
            if (str_content.size() <= 20) {
                if (let_string_values.count(str_content)) {
                    result += ' ';  // strip: let-assignment string
                } else {
                    result += '"';
                    result += str_content;
                    result += '"';
                }
            } else {
                result += ' ';
            }
            continue;
        }
        // Skip single-quoted string literals (always strip — not dict keys)
        if (code[i] == '\'') {
            i++;
            while (i < code.size() && code[i] != '\'') {
                if (code[i] == '\\' && i + 1 < code.size()) i++;
                i++;
            }
            if (i < code.size()) i++;
            result += ' ';
            continue;
        }
        // Check for identifier — if it matches a local variable name, erase it
        if (std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') {
            std::string word;
            while (i < code.size() &&
                   (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) {
                word += code[i]; i++;
            }
            if (local_vars.count(word)) {
                result += ' '; // preserve word boundary but erase the name
            } else {
                result += word;
            }
            continue;
        }
        result += code[i];
        i++;
    }
    return result;
}

// Helper: match keyword at word boundaries (not as substring of larger word)
static bool wordBoundaryMatch(const std::string& text, const std::string& word) {
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0 ||
            !std::isalnum(static_cast<unsigned char>(text[pos - 1])));
        size_t end = pos + word.size();
        bool right_ok = (end >= text.size() ||
            !std::isalnum(static_cast<unsigned char>(text[end])));
        if (left_ok && right_ok) return true;
        pos++;
    }
    return false;
}

// Co-occurrence synonym groups: intent keyword → code alternatives
// Unidirectional lookup. Synonym matches score 0.75 (not 1.0) to prevent inflation.
static const std::unordered_map<std::string, std::vector<std::string>> KEYWORD_SYNONYMS = {
    // Control flow verbs → code constructs
    {"iterate",   {"for", "loop", "foreach", "while", "range"}},
    {"loop",      {"for", "while", "foreach", "repeat", "iterate"}},
    {"execute",   {"run", "call", "invoke", "dispatch", "perform"}},
    {"check",     {"verify", "validate", "assert", "test", "ensure", "confirm"}},

    // I/O and data verbs
    {"load",      {"read", "open", "fetch", "parse", "ingest"}},
    {"store",     {"save", "write", "persist", "cache", "put"}},
    {"print",     {"output", "display", "write", "emit", "show", "format"}},
    {"log",       {"audit", "record", "trace", "track", "journal", "append"}},
    {"report",    {"summary", "summarize", "aggregate", "tally", "stats"}},

    // Data processing verbs
    {"evaluate",  {"check", "match", "test", "assess", "score", "judge"}},
    {"detect",    {"find", "search", "scan", "identify", "discover"}},
    {"validate",  {"check", "verify", "assert", "ensure", "confirm"}},
    {"parse",     {"decode", "deserialize", "extract", "tokenize", "read"}},
    {"transform", {"convert", "map", "format", "process"}},
    {"filter",    {"select", "where", "exclude", "pick"}},
    {"merge",     {"combine", "join", "concat", "append"}},
    {"sort",      {"order", "rank", "arrange"}},

    // CRUD verbs
    {"create",    {"new", "make", "build", "init", "generate", "construct"}},
    {"remove",    {"delete", "drop", "clear", "erase", "purge"}},
    {"send",      {"post", "emit", "dispatch", "push", "publish"}},
    {"receive",   {"get", "accept", "listen", "consume", "pull"}},

    // Testing and verification verbs
    {"verify",    {"check", "validate", "assert", "test", "tests", "confirm", "ensure"}},
    {"suite",     {"test", "tests", "spec", "cases", "bench", "run"}},
    {"correct",   {"valid", "right", "expected", "proper", "pass", "true"}},
};

// Anti-stuffing: strip unused variable assignments from intent analysis.
// Detects `let <name> = <expr>` where <name> never appears again in the body.
// This prevents dead code like `let judge_handle = create_judge()` from inflating
// keyword overlap when judge_handle is never read.
static std::string stripUnusedAssignments(const std::string& code) {
    // Collect all `let <name> = ...` with their line positions
    struct Assignment {
        std::string name;
        size_t line_start;
        size_t line_end;
    };
    std::vector<Assignment> assignments;

    // Find all let assignments and their line boundaries
    size_t i = 0;
    while (i < code.size()) {
        // Find start of current line
        size_t line_start = i;

        // Check for `let ` at current position (after optional whitespace)
        size_t j = i;
        while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) j++;

        if (j + 3 < code.size() && code.substr(j, 4) == "let ") {
            j += 4;
            while (j < code.size() && (code[j] == ' ' || code[j] == '\t')) j++;
            std::string name;
            while (j < code.size() &&
                   (std::isalnum(static_cast<unsigned char>(code[j])) || code[j] == '_')) {
                name += code[j]; j++;
            }
            // Only track assignments (not destructuring or declarations without =)
            size_t eq = j;
            while (eq < code.size() && (code[eq] == ' ' || code[eq] == '\t')) eq++;
            if (!name.empty() && name.size() >= 3 && eq < code.size() && code[eq] == '=') {
                // Find end of this line
                size_t line_end = i;
                while (line_end < code.size() && code[line_end] != '\n') line_end++;
                if (line_end < code.size()) line_end++; // include newline
                assignments.push_back({name, line_start, line_end});
            }
        }

        // Advance to next line
        while (i < code.size() && code[i] != '\n') i++;
        if (i < code.size()) i++;
    }

    if (assignments.empty()) return code;

    // Build comment-blanked version: replace comment chars with spaces to preserve
    // positions. This prevents an LLM from defeating dead-code detection by
    // mentioning the variable name in a comment.
    std::string search_code = code;
    for (size_t si = 0; si < search_code.size(); ) {
        if (search_code[si] == '/' && si + 1 < search_code.size() && search_code[si + 1] == '/') {
            while (si < search_code.size() && search_code[si] != '\n') { search_code[si] = ' '; si++; }
        } else if (search_code[si] == '#') {
            while (si < search_code.size() && search_code[si] != '\n') { search_code[si] = ' '; si++; }
        } else {
            si++;
        }
    }

    // For each assignment, check if the variable name appears elsewhere in the code
    std::unordered_set<size_t> lines_to_strip; // set of line_start positions to remove
    for (const auto& asgn : assignments) {
        // Search the comment-blanked code for variable usage
        bool found_elsewhere = false;
        size_t pos = 0;
        while (pos < search_code.size()) {
            // Skip the assignment line itself
            if (pos >= asgn.line_start && pos < asgn.line_end) {
                pos = asgn.line_end;
                continue;
            }
            // Check for the variable name at word boundary
            if ((std::isalpha(static_cast<unsigned char>(search_code[pos])) || search_code[pos] == '_')) {
                std::string word;
                size_t wstart = pos;
                while (pos < search_code.size() &&
                       (std::isalnum(static_cast<unsigned char>(search_code[pos])) || search_code[pos] == '_')) {
                    word += search_code[pos]; pos++;
                }
                if (word == asgn.name) {
                    // Check it's at a word boundary (not part of a larger identifier)
                    bool left_ok = (wstart == 0 ||
                        (!std::isalnum(static_cast<unsigned char>(search_code[wstart - 1])) && search_code[wstart - 1] != '_'));
                    bool right_ok = (pos >= search_code.size() ||
                        (!std::isalnum(static_cast<unsigned char>(search_code[pos])) && search_code[pos] != '_'));
                    if (left_ok && right_ok) {
                        found_elsewhere = true;
                        break;
                    }
                }
                continue;
            }
            pos++;
        }
        if (!found_elsewhere) {
            lines_to_strip.insert(asgn.line_start);
        }
    }

    if (lines_to_strip.empty()) return code;

    // Rebuild code without the stripped lines
    std::string result;
    result.reserve(code.size());
    i = 0;
    while (i < code.size()) {
        if (lines_to_strip.count(i)) {
            // Skip this line
            while (i < code.size() && code[i] != '\n') i++;
            if (i < code.size()) i++;
            result += ' '; // preserve word boundary
        } else {
            result += code[i];
            i++;
        }
    }
    return result;
}

// Anti-stuffing: truncate compound identifiers (5+ underscore parts) to first 3 parts.
// Natural code maxes at ~3 parts (compute_risk_score). Stuffed names pack 6+.
static std::string truncateCompoundIdentifiers(const std::string& code) {
    std::string result;
    result.reserve(code.size());
    size_t i = 0;
    while (i < code.size()) {
        if (std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') {
            std::string ident;
            while (i < code.size() &&
                   (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_')) {
                ident += code[i]; i++;
            }
            // Count underscore-separated parts
            int parts = 1;
            for (char c : ident) if (c == '_') parts++;
            if (parts >= 5) {
                // Keep only first 3 parts
                int underscores_seen = 0;
                for (size_t j = 0; j < ident.size(); j++) {
                    if (ident[j] == '_') {
                        underscores_seen++;
                        if (underscores_seen >= 3) {
                            ident = ident.substr(0, j);
                            break;
                        }
                    }
                }
            }
            result += ident;
        } else {
            result += code[i]; i++;
        }
    }
    return result;
}

// Helper: check keyword overlap between intent and body+name
// Returns ratio of matched keywords (direct = 1.0, synonym = 0.75).
// matched_out: comma-separated list of matched keywords for error messages.
static double keywordOverlap(const std::vector<std::string>& keywords,
                              const std::string& body, const std::string& name,
                              std::string& missing_out,
                              std::string& matched_out,
                              int max_missing = 5) {
    if (keywords.empty()) return 1.0;
    std::string lower_body = body;
    std::transform(lower_body.begin(), lower_body.end(), lower_body.begin(), ::tolower);
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    double found = 0.0;
    missing_out.clear();
    matched_out.clear();
    int shown_missing = 0;
    int shown_matched = 0;

    for (const auto& kw : keywords) {
        // Direct match (full weight = 1.0)
        if (wordBoundaryMatch(lower_body, kw) ||
            wordBoundaryMatch(lower_name, kw)) {
            found += 1.0;
            if (shown_matched < 8) {
                if (!matched_out.empty()) matched_out += ", ";
                matched_out += kw;
                shown_matched++;
            }
            continue;
        }

        // Synonym match (reduced weight = 0.75)
        bool syn_found = false;
        auto syn_it = KEYWORD_SYNONYMS.find(kw);
        if (syn_it != KEYWORD_SYNONYMS.end()) {
            for (const auto& syn : syn_it->second) {
                if (wordBoundaryMatch(lower_body, syn) ||
                    wordBoundaryMatch(lower_name, syn)) {
                    found += 0.75;
                    if (shown_matched < 8) {
                        if (!matched_out.empty()) matched_out += ", ";
                        matched_out += kw + " (via '" + syn + "')";
                        shown_matched++;
                    }
                    syn_found = true;
                    break;  // first synonym match wins
                }
            }
        }
        if (syn_found) continue;

        // Plural/singular match (weight = 0.9)
        // "actions" matches "action", "test" matches "tests"
        {
            bool plural_found = false;
            std::string plural_variant;
            if (kw.size() >= 4 && kw.back() == 's') {
                // Try singular: "actions" → "action"
                plural_variant = kw.substr(0, kw.size() - 1);
            } else if (kw.size() >= 3 && kw.back() != 's') {
                // Try plural: "action" → "actions"
                plural_variant = kw + "s";
            }
            if (!plural_variant.empty() &&
                (wordBoundaryMatch(lower_body, plural_variant) ||
                 wordBoundaryMatch(lower_name, plural_variant))) {
                found += 0.9;
                if (shown_matched < 8) {
                    if (!matched_out.empty()) matched_out += ", ";
                    matched_out += kw + " (~'" + plural_variant + "')";
                    shown_matched++;
                }
                plural_found = true;
            }
            if (plural_found) continue;
        }

        // Not found
        if (shown_missing < max_missing) {
            if (!missing_out.empty()) missing_out += ", ";
            missing_out += kw;
            shown_missing++;
        } else if (shown_missing == max_missing) {
            missing_out += "...";
            shown_missing++;
        }
    }
    return found / static_cast<double>(keywords.size());
}

// Helper: check if intent text is trivially vague
static bool isTrivialIntent(const std::string& intent) {
    std::string lower = intent;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const std::vector<std::string> trivial = {
        "does stuff", "handles data", "processes input", "does things",
        "placeholder", "todo", "implement later", "not implemented",
        "stub", "dummy", "tbd", "fixme"
    };
    for (const auto& pat : trivial) {
        if (lower.find(pat) != std::string::npos) return true;
    }
    return false;
}

std::string GovernanceEngine::checkIntentValidation(
    const std::string& function_name, const std::string& llm_intent,
    const std::string& body, int line) {

    auto& cfg = rules().code_quality.intent_validation;
    if (!cfg.enabled) return "";
    clearTrace();
    if (cfg.mode == "agent") {
        addTrace("mode=agent — skipping static validation");
        recordPass("code_quality.intent_validation", cfg.level);
        return "";
    }

    // Check exemptions
    for (const auto& exempt : cfg.exempt_functions) {
        if (function_name == exempt) {
            addTrace(fmt::format("function '{}' is exempt — skipping", function_name));
            return "";
        }
    }

    int body_lines = 0;
    for (char c : body) if (c == '\n') body_lines++;

    // --- Tier 1: Owner's per-function intent (ground truth) ---
    auto it = cfg.function_intents.find(function_name);
    if (it != cfg.function_intents.end()) {
        addTrace(fmt::format("Tier 1: owner intent found for '{}' in function_intents", function_name));
        const std::string& owner_intent = it->second;

        // F13: Empty owner intent is invalid
        if (owner_intent.empty()) {
            return enforce("code_quality.intent_validation", EnforcementLevel::ADVISORY,
                fmt::format("Owner intent for '{}' is empty in function_intents.\n"
                    "  Provide a meaningful intent description in govern.json.",
                    function_name));
        }

        // Trivial owner intent
        if (isTrivialIntent(owner_intent)) {
            return enforce("code_quality.intent_validation", EnforcementLevel::ADVISORY,
                fmt::format("Owner intent for '{}' is too vague: \"{}\"\n"
                    "  function_intents in govern.json should describe what the function does.",
                    function_name, owner_intent));
        }

        // Body vs owner intent (configured level — this is ground truth)
        // Strip: 1) function declaration line (param names create false matches)
        //        2) comments and string literals (keyword stuffing vectors)
        std::string inner_body = body;
        size_t first_brace = inner_body.find('{');
        if (first_brace != std::string::npos && first_brace + 1 < inner_body.size()) {
            inner_body = inner_body.substr(first_brace + 1);
        }
        inner_body = stripUnusedAssignments(inner_body);
        inner_body = stripNonCodeContent(inner_body);
        inner_body = truncateCompoundIdentifiers(inner_body);
        addTrace("anti-stuffing: stripped unused assignments, comments/strings, truncated compound identifiers");
        auto keywords = extractIntentKeywords(owner_intent);
        if (keywords.size() >= 3) {
            std::string kw_list;
            for (size_t ki = 0; ki < keywords.size() && ki < 8; ki++) {
                if (ki > 0) kw_list += ", ";
                kw_list += keywords[ki];
            }
            if (keywords.size() > 8) kw_list += ", ...";
            addTrace(fmt::format("extracted {} keywords from owner intent: [{}]", keywords.size(), kw_list));
            std::string missing, matched;
            // Don't match against function_name — owner defined that slot, so name match is circular
            double overlap = keywordOverlap(keywords, inner_body, "", missing, matched);
            double min_overlap = std::max(0.3, 2.0 / static_cast<double>(keywords.size()));
            addTrace(fmt::format("keyword overlap={:.0f}% (threshold={:.0f}%), matched=[{}], missing=[{}]",
                overlap * 100, min_overlap * 100,
                matched.empty() ? "none" : matched,
                missing.empty() ? "none" : missing));
            if (overlap < min_overlap && keywords.size() >= 3) {
                return enforce("code_quality.intent_validation", cfg.level,
                    fmt::format("Intent mismatch on '{}': {:.0f}% overlap (need {:.0f}%).\n"
                        "  Owner requires: \"{}\"\n"
                        "  Matched: {}\n"
                        "  Missing: {}\n"
                        "  Note: Only executable code counts. Comments, string literals\n"
                        "  assigned to variables, and local variable names are stripped\n"
                        "  before matching.\n"
                        "  Hint: Use intent keywords in function calls and identifiers.\n"
                        "  Snake_case like load_data() matches 'load'. Common synonyms\n"
                        "  of programming verbs also count (e.g., 'for' matches 'iterate').\n"
                        "  Example: if intent says 'parse JSON and validate fields', use\n"
                        "  json.parse(), validate_fields(), or field_check() in your code.",
                        function_name, overlap * 100, min_overlap * 100,
                        owner_intent,
                        matched.empty() ? "(none)" : matched,
                        missing.empty() ? "(none)" : missing));
            }
        }

        // Side-effect detection: flag I/O operations not mentioned in owner intent
        // Catches dual-purpose attacks where code does the right thing PLUS exfiltrates
        {
            std::string intent_lower = owner_intent;
            std::transform(intent_lower.begin(), intent_lower.end(), intent_lower.begin(), ::tolower);
            std::string body_lower = inner_body;
            std::transform(body_lower.begin(), body_lower.end(), body_lower.begin(), ::tolower);

            // I/O and escape operations that should be mentioned in intent if present
            static const std::vector<std::pair<std::string, std::string>> side_effects = {
                {"print(", "print/output"},
                {"io.write(", "I/O write"},
                {"io.write_error(", "stderr write"},
                {"file.write(", "file write"},
                {"file.append(", "file append"},
                {"file.delete(", "file delete"},
                {"http.get(", "network call"},
                {"http.post(", "network call"},
                {"http.request(", "network call"},
                {"env.set_var(", "env modification"},
                {"env.get(", "env read"},
                {"process.exec(", "process execution"},
                {"process.run(", "process execution"},
                {"<<python", "polyglot escape (python)"},
                {"<<javascript", "polyglot escape (javascript)"},
                {"<<shell", "polyglot escape (shell)"},
                {"<<go", "polyglot escape (go)"},
                {"<<ruby", "polyglot escape (ruby)"},
                {"<<rust", "polyglot escape (rust)"},
                {"<<cpp", "polyglot escape (cpp)"},
                {"<<c++", "polyglot escape (cpp)"},
                {"<<csharp", "polyglot escape (csharp)"},
                {"<<nim", "polyglot escape (nim)"},
                {"<<php", "polyglot escape (php)"},
                {"<<julia", "polyglot escape (julia)"},
                {"<<zig", "polyglot escape (zig)"},
            };

            // Intent keywords that signal I/O or polyglot is expected
            static const std::vector<std::string> io_intent_words = {
                "print", "output", "write", "log", "display", "show", "file",
                "network", "http", "send", "fetch", "stdout", "stderr", "report",
                "format", "emit", "publish", "python", "javascript", "shell",
                "polyglot", "external", "execute", "script", "env", "environment"
            };

            bool intent_mentions_io = false;
            for (const auto& w : io_intent_words) {
                if (wordBoundaryMatch(intent_lower, w)) {
                    intent_mentions_io = true;
                    break;
                }
            }

            addTrace(fmt::format("side-effect check: intent_mentions_io={}", intent_mentions_io ? "yes" : "no"));
            if (!intent_mentions_io) {
                std::vector<std::string> found_effects;
                for (const auto& [pattern, label] : side_effects) {
                    if (body_lower.find(pattern) != std::string::npos) {
                        found_effects.push_back(label);
                    }
                }
                if (!found_effects.empty()) {
                    addTrace(fmt::format("detected {} undeclared side effects", found_effects.size()));
                    std::string effects_str;
                    for (const auto& e : found_effects) {
                        if (!effects_str.empty()) effects_str += ", ";
                        effects_str += e;
                    }
                    return enforce("code_quality.intent_validation", cfg.level,
                        fmt::format("Unexpected side effects in '{}': {} found but owner intent "
                            "doesn't mention I/O.\n"
                            "  Owner requires: \"{}\"\n"
                            "  Detected: {}\n"
                            "  Functions should only perform operations described in their intent.",
                            function_name, found_effects.size(), owner_intent, effects_str));
                }
            }
        }

        // Rubber-stamp detection: owner intent mentions rejection/validation but code has no branches
        {
            std::string intent_lower = owner_intent;
            std::transform(intent_lower.begin(), intent_lower.end(), intent_lower.begin(), ::tolower);
            static const std::vector<std::string> rejection_words = {
                "reject", "deny", "block", "fail", "error", "invalid", "validate",
                "check", "verify", "ensure", "must", "require", "prohibit", "prevent"
            };
            bool intent_requires_rejection = false;
            for (const auto& w : rejection_words) {
                if (intent_lower.find(w) != std::string::npos) {
                    intent_requires_rejection = true;
                    break;
                }
            }
            addTrace(fmt::format("rubber-stamp check: intent_requires_rejection={}", intent_requires_rejection ? "yes" : "no"));
            if (intent_requires_rejection) {
                // Use stripped body — raw body lets comments with "if" defeat the check
                std::string stripped_lower = inner_body;
                std::transform(stripped_lower.begin(), stripped_lower.end(), stripped_lower.begin(), ::tolower);
                bool has_branch = (stripped_lower.find("if ") != std::string::npos ||
                                   stripped_lower.find("if(") != std::string::npos ||
                                   stripped_lower.find("match ") != std::string::npos ||
                                   stripped_lower.find("throw ") != std::string::npos);
                addTrace(fmt::format("rubber-stamp: has_branch={}", has_branch ? "yes" : "no"));
                if (!has_branch) {
                    // Check for delegation: calling a validator function
                    static const std::vector<std::string> validation_calls = {
                        "validate", "check", "verify", "ensure", "assert", "reject", "deny"
                    };
                    bool delegates_to_validator = false;
                    for (const auto& vc : validation_calls) {
                        if (stripped_lower.find(vc + "(") != std::string::npos ||
                            stripped_lower.find(vc + "_") != std::string::npos) {
                            delegates_to_validator = true;
                            break;
                        }
                    }
                    if (!delegates_to_validator) {
                        return enforce("code_quality.intent_validation", cfg.level,
                            fmt::format("Rubber-stamp detected in '{}': owner intent requires validation/rejection "
                                "but function has no conditional branches or validator delegation.\n"
                                "  Owner requires: \"{}\"\n"
                                "  Functions that must reject invalid input need if/match/throw logic\n"
                                "  or must delegate to a validation function.\n"
                                "  Bad:  fn validate(x) {{ return x }}  // always passes\n"
                                "  Good: fn validate(x) {{\n"
                                "      if type(x) != \"dict\" {{ return null }}\n"
                                "      if !x.has(\"id\") {{ return null }}\n"
                                "      return x\n"
                                "  }}",
                                function_name, owner_intent));
                    }
                }
            }
        }

        // LLM @intent vs owner intent (advisory — is the LLM confused about requirements?)
        bool diverged = false;
        if (!llm_intent.empty()) {
            addTrace(fmt::format("checking LLM @intent vs owner intent divergence"));
            auto owner_kw = extractIntentKeywords(owner_intent);
            auto llm_kw = extractIntentKeywords(llm_intent);
            if (owner_kw.size() >= 3 && llm_kw.size() >= 3) {
                std::string llm_lower = llm_intent;
                std::transform(llm_lower.begin(), llm_lower.end(), llm_lower.begin(), ::tolower);

                std::string missing, matched_discard;
                double llm_overlap = keywordOverlap(owner_kw, llm_lower, "", missing, matched_discard);
                addTrace(fmt::format("LLM-vs-owner overlap={:.0f}% (divergence threshold=15%)", llm_overlap * 100));
                if (llm_overlap < 0.15) {
                    diverged = true;
                    enforce("code_quality.intent_validation", EnforcementLevel::ADVISORY,
                        fmt::format("Intent conflict on '{}': LLM's @intent diverges from owner's.\n"
                            "  Owner requires: \"{}\"\n"
                            "  LLM claims: \"{}\"\n"
                            "  The agent may have misunderstood the requirement.",
                            function_name, owner_intent, llm_intent));
                }
            }
        }

        // Only record pass if no divergence advisory was emitted
        if (!diverged) {
            recordPass("code_quality.intent_validation", cfg.level);
        }
        return "";
    }

    // --- Tier 2: Project-wide intent (advisory — broad context) ---
    if (!cfg.project_intent.empty()) {
        addTrace("Tier 2: checking project-wide intent");
        auto proj_kw = extractIntentKeywords(cfg.project_intent);
        if (proj_kw.size() >= 3 && body_lines >= cfg.min_function_lines) {
            // Strip comments/strings/vars before matching (same as Tier 1)
            std::string stripped_body = stripNonCodeContent(body);
            std::string lower_body = stripped_body;
            std::transform(lower_body.begin(), lower_body.end(), lower_body.begin(), ::tolower);
            std::string lower_name = function_name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

            int found = 0;
            for (const auto& kw : proj_kw) {
                if (wordBoundaryMatch(lower_body, kw) ||
                    wordBoundaryMatch(lower_name, kw)) {
                    found++;
                }
            }
            addTrace(fmt::format("project keyword overlap: {}/{} matched", found, proj_kw.size()));
            // If zero project keywords match, this function might not belong
            if (found == 0 && proj_kw.size() >= 4) {
                // Format project keywords for display
                std::string kw_list;
                for (size_t ki = 0; ki < proj_kw.size() && ki < 12; ki++) {
                    if (ki > 0) kw_list += ", ";
                    kw_list += proj_kw[ki];
                }
                if (proj_kw.size() > 12) kw_list += ", ...";

                // Determine enforcement level and rule name based on cooperation
                EnforcementLevel eff_level;
                std::string rule;
                if (cfg.function_intents.empty()) {
                    // No function_intents defined — pure advisory
                    eff_level = EnforcementLevel::ADVISORY;
                    rule = "code_quality.intent_validation";
                } else if (!llm_intent.empty()) {
                    // Has @intent — cooperating with governance.
                    // Downgrade to advisory; Tier 3 will validate the self-declared intent.
                    eff_level = EnforcementLevel::ADVISORY;
                    rule = "code_quality.intent_validation.self_declared";
                } else {
                    // No @intent, no owner intent, zero project overlap — suspicious.
                    // Keep escalation to configured level.
                    eff_level = cfg.level;
                    rule = "code_quality.intent_validation";
                }

                std::string msg;
                if (!llm_intent.empty()) {
                    // Downgraded — advisory message for supporting function
                    msg = enforce(rule, eff_level,
                        fmt::format("Function '{}' has no project keyword overlap (advisory).\n"
                            "  This is a supporting function — checked at reduced weight.\n"
                            "  To reduce risk score: call functions or use identifiers related to:\n"
                            "  {}",
                            function_name, kw_list));
                } else {
                    std::string guidance = fmt::format("\n\n  How to fix (any one of these):\n"
                        "  - Add /// @intent \"...\" above the function to declare its purpose\n"
                        "    (downgrades this to advisory — intent is still validated)\n"
                        "  - Use variable/function names that match project keywords\n"
                        "  - Call functions whose names overlap with the project description\n"
                        "  - If this function doesn't belong, move its logic into a function\n"
                        "    that IS listed in function_intents\n"
                        "  - govern.json is signed — you cannot modify function_intents\n"
                        "  Project keywords: {}", kw_list);
                    msg = enforce(rule, eff_level,
                        fmt::format("Function '{}' has no keyword overlap with project intent.\n"
                            "  Project: \"{}\"{}",
                            function_name, cfg.project_intent, guidance));
                }
                if (!msg.empty()) return msg;
            }
        }

        // Side-effect detection at project level — check for operations the project
        // explicitly prohibits (e.g., "no network calls, no credential access")
        {
            std::string proj_lower = cfg.project_intent;
            std::transform(proj_lower.begin(), proj_lower.end(), proj_lower.begin(), ::tolower);
            // Use raw body (not stripped) for prohibition matching — stripNonCodeContent
            // removes parameter names like 'env', which would defeat env.get( detection
            std::string raw_body_lower = body;
            std::transform(raw_body_lower.begin(), raw_body_lower.end(), raw_body_lower.begin(), ::tolower);

            // All polyglot languages that could hide prohibited operations
            static const std::vector<std::string> all_polyglot = {
                "<<python", "<<javascript", "<<shell", "<<go", "<<ruby", "<<rust",
                "<<cpp", "<<c++", "<<csharp", "<<nim", "<<php", "<<julia", "<<zig"
            };

            // Check project-level prohibitions
            std::vector<std::pair<std::string, std::vector<std::string>>> prohibitions = {
                {"no network", {"http.get(", "http.post(", "http.request("}},
                {"no credential", {"env.get("}},
                {"no file", {"file.write(", "file.append(", "file.delete("}},
            };
            // Append all polyglot escapes to each prohibition
            for (auto& [prohibition, patterns] : prohibitions) {
                for (const auto& pl : all_polyglot) {
                    patterns.push_back(pl);
                }
            }
            for (const auto& [prohibition, patterns] : prohibitions) {
                if (proj_lower.find(prohibition) != std::string::npos) {
                    for (const auto& pat : patterns) {
                        if (raw_body_lower.find(pat) != std::string::npos) {
                            EnforcementLevel eff_level = cfg.function_intents.empty()
                                ? EnforcementLevel::ADVISORY : cfg.level;
                            std::string msg2 = enforce("code_quality.intent_validation", eff_level,
                                fmt::format("Function '{}' violates project prohibition.\n"
                                    "  Project states: \"{}\"\n"
                                    "  Detected: '{}' in function body.",
                                    function_name, cfg.project_intent, pat));
                            if (!msg2.empty()) return msg2;
                        }
                    }
                }
            }
        }
    }

    // --- Tier 3: LLM's @intent only (advisory — self-declared, lower trust) ---
    if (!llm_intent.empty()) {
        addTrace("Tier 3: self-declared @intent (advisory)");
        if (isTrivialIntent(llm_intent)) {
            addTrace("trivial @intent detected");
            return enforce("code_quality.intent_validation.self_declared", EnforcementLevel::ADVISORY,
                fmt::format("Trivial @intent on '{}': \"{}\"\n"
                    "  Intent declarations must be specific enough to validate against.",
                    function_name, llm_intent));
        }

        auto keywords = extractIntentKeywords(llm_intent);
        if (keywords.size() >= 3) {
            // Apply anti-stuffing pipeline (same as Tier 1) — raw body lets LLMs
            // inflate overlap with dead assignments and compound identifiers
            std::string t3_body = body;
            t3_body = stripUnusedAssignments(t3_body);
            t3_body = stripNonCodeContent(t3_body);
            t3_body = truncateCompoundIdentifiers(t3_body);

            std::string missing, matched;
            double overlap = keywordOverlap(keywords, t3_body, function_name, missing, matched);
            double t3_min_overlap = std::max(0.3, 2.0 / static_cast<double>(keywords.size()));
            addTrace(fmt::format("Tier 3 overlap={:.0f}% (threshold={:.0f}%), matched=[{}], missing=[{}]",
                overlap * 100, t3_min_overlap * 100,
                matched.empty() ? "none" : matched,
                missing.empty() ? "none" : missing));
            if (overlap < t3_min_overlap) {
                return enforce("code_quality.intent_validation.self_declared", EnforcementLevel::ADVISORY,
                    fmt::format("Self-declared intent mismatch on '{}': {:.0f}% overlap (need {:.0f}%).\n"
                        "  LLM claims: \"{}\"\n"
                        "  Matched: {}\n"
                        "  Missing: {}\n"
                        "  Status: advisory — adds to risk score but does not block on its own.\n"
                        "  This function is not in function_intents — checked at reduced weight.\n"
                        "  To reduce score: use missing keywords as identifiers in executable code,\n"
                        "  not in comments or variable names.",
                        function_name, overlap * 100, t3_min_overlap * 100,
                        llm_intent,
                        matched.empty() ? "(none)" : matched,
                        missing.empty() ? "(none)" : missing));
            }
        }

        recordPass("code_quality.intent_validation.self_declared", cfg.level);
        return "";
    }

    // --- Tier 4: Nothing declared ---
    addTrace("Tier 4: no intent declared");
    // Missing intent only matters if owner defined function_intents at all
    if (!cfg.function_intents.empty() && cfg.required && body_lines >= cfg.min_function_lines) {
        return enforce("code_quality.intent_validation", cfg.missing_level,
            fmt::format("Function '{}' has no intent declared ({} lines).\n"
                "  Add a comment before the function:\n"
                "  /// @intent \"Load data from file, parse JSON, return array of records\"\n\n"
                "  The @intent must describe what the function actually does.\n"
                "  It is matched against the function body — use specific verbs and nouns\n"
                "  that appear in your code (e.g., \"parse\", \"validate\", \"compute\").",
                function_name, body_lines));
    }

    recordPass("code_quality.intent_validation", cfg.level);
    return "";
}

std::string GovernanceEngine::checkIncompleteLogic(const std::string& code, int line, const std::string& source_file) {
    auto& cfg = rules().code_quality.no_incomplete_logic;
    if (!cfg.enabled) return "";
    clearTrace();
    auto& pats = cfg.patterns.empty() ? DEFAULT_INCOMPLETE_LOGIC_PATTERNS : cfg.patterns;

    std::vector<std::string> active_pats;
    for (const auto& p : pats) active_pats.push_back(p);
    for (const auto& p : cfg.custom_patterns) active_pats.push_back(p);

    std::string found = searchPatterns(code, active_pats, !cfg.case_sensitive);
    if (!found.empty()) {
        // Check suppressions: format is "file_glob" or "category:file_glob"
        if (!cfg.suppressions.empty() && !source_file.empty()) {
            std::string basename = source_file;
            auto slash = basename.rfind('/');
            if (slash != std::string::npos) basename = basename.substr(slash + 1);
            for (const auto& s : cfg.suppressions) {
                // Simple glob: *.naab, test_*.naab, validators.naab
                std::string glob = s;
                auto colon = s.find(':');
                if (colon != std::string::npos) glob = s.substr(colon + 1);
                // Match: exact match, or wildcard prefix match
                if (glob == basename) { recordPass("code_quality.no_incomplete_logic", cfg.level); return ""; }
                if (glob.size() > 1 && glob[0] == '*') {
                    std::string suffix = glob.substr(1);
                    if (basename.size() >= suffix.size() &&
                        basename.compare(basename.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        recordPass("code_quality.no_incomplete_logic", cfg.level);
                        return "";
                    }
                }
            }
        }
        return enforce("code_quality.no_incomplete_logic", cfg.level,
            formatError(cfg.level, fmt::format("Incomplete logic detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_incomplete_logic",
                "This code has logic gaps — shortcuts or lazy implementation.\n"
                "Every function should DO something meaningful:\n"
                "  - Error handlers must log or re-raise, never silently swallow\n"
                "  - Return values must be computed, not hardcoded\n"
                "  - Loops must process data, not immediately break\n\n"
                "  NAAb-specific fixes:\n"
                "  - catch (e) { } → catch (e) { print(\"Error: \" + string(e)); throw e }\n"
                "  - fn validate(x) { return true } → add real checks\n"
                "  - score = 0.85 → score = len(matches) / len(total)",
                "catch (e) { }  // empty catch swallows error\n"
                "fn validate(x) { return true }  // always passes\n"
                "let score = 0.85  // hardcoded",
                "catch (e) { print(\"Error: \" + string(e)); throw e }\n"
                "fn validate(x) {\n"
                "    if x == null { return false }\n"
                "    if len(x) == 0 { return false }\n"
                "    return true\n"
                "}\n"
                "let score = len(matches) / len(total)"));
    }
    recordPass("code_quality.no_incomplete_logic", cfg.level);
    return "";
}

// --- Function Contract Check ---

std::string GovernanceEngine::checkFunctionContract(
    const std::string& func_name,
    const std::string& result_str,
    const std::string& result_type,
    int line,
    const std::string& source_file) {

    setCheckContext(source_file, line);

    clearTrace();
    auto it = rules().contracts.functions.find(func_name);
    if (it == rules().contracts.functions.end()) return "";

    const auto& contract = it->second;
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;
    addTrace(fmt::format("contract found for '{}' in govern.json", func_name));

    auto make_err = [&](const std::string& detail) -> std::string {
        // Build contract-specific examples
        std::string bad_ex, good_ex;
        if (detail.find("return_type") != std::string::npos && detail.find("dict") != std::string::npos) {
            bad_ex = "return null  // or return 42";
            good_ex = "return {\"id\": computed_id, \"type\": entity_type}";
        } else if (detail.find("missing required key") != std::string::npos) {
            bad_ex = "return {\"id\": 1}  // missing required keys";
            std::string keys_str;
            for (const auto& k : contract.return_keys) {
                if (!keys_str.empty()) keys_str += ", ";
                keys_str += "\"" + k + "\": value";
            }
            good_ex = "return {" + keys_str + "}";
        } else if (detail.find("return_one_of") != std::string::npos) {
            bad_ex = "return \"invalid_value\"";
            std::string vals;
            for (const auto& v : contract.return_one_of) {
                if (!vals.empty()) vals += ", ";
                vals += "\"" + v + "\"";
            }
            good_ex = "return one of: " + vals;
        } else if (detail.find("match pattern") != std::string::npos) {
            bad_ex = "return \"invalid-trace-id\"  // error sentinel, not real data";
            good_ex = "if !valid { throw \"Invalid trace ID\" }\nreturn trace_id  // only valid values";
        } else if (detail.find("return_keys_non_null") != std::string::npos ||
                   detail.find("return_keys_non_empty") != std::string::npos) {
            bad_ex = "return {\"services\": [], \"stats\": {}}  // keys present but empty";
            good_ex = "return {\"services\": [real_data], \"stats\": {\"count\": n}}  // substantive values";
        } else if (detail.find("non-null") != std::string::npos) {
            bad_ex = "return null";
            good_ex = "return computed_value  // ensure non-null";
        } else if (detail.find("below minimum") != std::string::npos) {
            bad_ex = "return -5  // below minimum";
            good_ex = "return int(math.max(" + std::to_string(contract.return_min) + ", result))";
        }

        // Build help text with contract spec
        std::string help = contract.description.empty()
            ? "Function return value did not match contract"
            : contract.description;
        if (!contract.return_type.empty())
            help += "\n  Expected return type: " + contract.return_type;
        if (!contract.return_keys.empty()) {
            help += "\n  Required keys: ";
            for (size_t i = 0; i < contract.return_keys.size(); i++) {
                if (i > 0) help += ", ";
                help += contract.return_keys[i];
            }
        }
        if (!contract.return_one_of.empty()) {
            help += "\n  Valid values: ";
            for (size_t i = 0; i < contract.return_one_of.size(); i++) {
                if (i > 0) help += ", ";
                help += "\"" + contract.return_one_of[i] + "\"";
            }
        }
        if (contract.has_return_min) {
            help += fmt::format("\n  Minimum value: {}", contract.return_min);
        }
        if (contract.has_return_max) {
            help += fmt::format("\n  Maximum value: {}", contract.return_max);
        }

        return enforce("contracts." + func_name, level,
            formatError(level,
                fmt::format("Contract violation for '{}': {}", func_name, detail),
                line > 0 ? fmt::format("line {}", line) : "",
                "contracts",
                help,
                bad_ex, good_ex));
    };

    // return_not_null
    if (contract.return_not_null && result_type == "null") {
        addTrace("return_not_null: FAIL — got null");
        return make_err("expected non-null return, got null");
    }
    if (contract.return_not_null) addTrace("return_not_null: PASS");

    // return_type
    if (!contract.return_type.empty() && result_type != contract.return_type) {
        addTrace(fmt::format("return_type: FAIL — expected '{}', got '{}'", contract.return_type, result_type));
        return make_err(fmt::format("expected return_type '{}', got '{}'",
            contract.return_type, result_type));
    }
    if (!contract.return_type.empty()) addTrace(fmt::format("return_type: PASS — '{}'", result_type));

    // Numeric checks — single parse, wrapped in try-catch for safety
    bool is_numeric = (result_type == "int" || result_type == "float");
    bool needs_numeric = is_numeric && (contract.has_return_range ||
                                         contract.has_return_min ||
                                         contract.has_return_max);

    if (needs_numeric) {
        double val;
        try {
            val = std::stod(result_str);
        } catch (...) {
            return make_err(fmt::format("return value '{}' is not parseable as a number", result_str));
        }

        if (contract.has_return_range &&
            (val < contract.return_range_min || val > contract.return_range_max)) {
            return make_err(fmt::format("return value {} outside range [{}, {}]",
                result_str, contract.return_range_min, contract.return_range_max));
        }

        if (contract.has_return_min && val < contract.return_min) {
            return make_err(fmt::format("return value {} below minimum {}",
                result_str, contract.return_min));
        }

        if (contract.has_return_max && val > contract.return_max) {
            return make_err(fmt::format("return value {} above maximum {}",
                result_str, contract.return_max));
        }
    }

    // return_one_of
    if (!contract.return_one_of.empty()) {
        bool found = false;
        for (const auto& opt : contract.return_one_of) {
            if (result_str == opt) { found = true; break; }
        }
        if (!found) {
            addTrace(fmt::format("return_one_of: FAIL — '{}' not in allowed values", result_str));
            std::string opts;
            for (const auto& opt : contract.return_one_of) {
                if (!opts.empty()) opts += ", ";
                opts += "\"" + opt + "\"";
            }
            return make_err(fmt::format("return value \"{}\" not in [{}]", result_str, opts));
        }
    }

    // return_non_empty
    if (contract.return_non_empty) {
        bool empty = result_str.empty() || result_str == "[]" || result_str == "{}"
                     || result_str == "\"\"" || result_type == "null";
        if (empty) {
            addTrace("return_non_empty: FAIL — empty value");
            return make_err("expected non-empty return value");
        }
    }

    // return_matches (regex validation for string returns)
    if (!contract.return_matches.empty() && result_type == "string") {
        try {
            std::regex re(contract.return_matches);
            if (!std::regex_match(result_str, re)) {
                return make_err(fmt::format(
                    "return value \"{}\" does not match pattern /{}/",
                    result_str, contract.return_matches));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: contract regex pattern failed: %s\n", re.what());
        }
    }

    // return_keys (for dicts)
    if (!contract.return_keys.empty() && result_type == "dict") {
        for (const auto& key : contract.return_keys) {
            // Match key as a proper dict key, not as a substring of another key or value
            // NAAb dict toString format: {"key1": val1, "key2": val2}
            std::string quoted_key = "\"" + key + "\":";
            if (result_str.find(quoted_key) == std::string::npos) {
                return make_err(fmt::format("return dict missing required key '{}'", key));
            }
        }
    }

    // return_keys_non_null / return_keys_non_empty (value validation for dict keys)
    if ((contract.return_keys_non_null || contract.return_keys_non_empty) &&
        result_type == "dict" && !contract.return_keys.empty()) {
        for (const auto& key : contract.return_keys) {
            std::string quoted_key = "\"" + key + "\":";
            auto pos = result_str.find(quoted_key);
            if (pos == std::string::npos) continue; // already caught by return_keys
            auto val_start = pos + quoted_key.size();
            while (val_start < result_str.size() && result_str[val_start] == ' ') val_start++;
            if (contract.return_keys_non_null) {
                if (result_str.compare(val_start, 4, "null") == 0) {
                    return make_err(fmt::format(
                        "return dict key '{}' is null (return_keys_non_null requires non-null values)", key));
                }
            }
            if (contract.return_keys_non_empty) {
                if (result_str.compare(val_start, 4, "null") == 0 ||
                    result_str.compare(val_start, 2, "[]") == 0 ||
                    result_str.compare(val_start, 2, "{}") == 0 ||
                    result_str.compare(val_start, 2, "\"\"") == 0) {
                    return make_err(fmt::format(
                        "return dict key '{}' is empty (return_keys_non_empty requires substantive values)", key));
                }
            }
        }
    }

    // return_length_min / return_length_max
    if (contract.return_length_min >= 0 || contract.return_length_max >= 0) {
        int length = -1;
        if (result_type == "string") {
            length = static_cast<int>(result_str.size());
        } else if (result_type == "array") {
            if (result_str == "[]") {
                length = 0;
            } else {
                // Depth-aware counting: only count commas at top-level (depth 1)
                // Handles nested arrays, dicts, and strings with commas correctly
                length = 1;
                int depth = 0;
                bool in_string = false;
                char prev = 0;
                for (char c : result_str) {
                    if (c == '"' && prev != '\\') in_string = !in_string;
                    if (!in_string) {
                        if (c == '[' || c == '{') depth++;
                        if (c == ']' || c == '}') depth--;
                        if (c == ',' && depth == 1) length++;
                    }
                    prev = c;
                }
            }
        }
        if (length >= 0) {
            if (contract.return_length_min >= 0 && length < contract.return_length_min) {
                return make_err(fmt::format("return length {} below minimum {}",
                    length, contract.return_length_min));
            }
            if (contract.return_length_max >= 0 && length > contract.return_length_max) {
                return make_err(fmt::format("return length {} above maximum {}",
                    length, contract.return_length_max));
            }
        }
    }

    return "";
}

// --- Input Contract Validation (v4) ---

std::string GovernanceEngine::checkFunctionInputContract(
    const std::string& func_name,
    const std::vector<std::string>& arg_types,
    int line) {

    if (!rules().contracts.validate_inputs) return "";

    auto it = rules().contracts.functions.find(func_name);
    if (it == rules().contracts.functions.end()) return "";

    const auto& contract = it->second;
    if (contract.params.empty()) return "";

    // Check argument count
    if (arg_types.size() != contract.params.size()) {
        std::string msg = "Contract violation [" + func_name + "]: expected " +
                          std::to_string(contract.params.size()) + " arguments, got " +
                          std::to_string(arg_types.size());
        if (line > 0) msg += " (line " + std::to_string(line) + ")";

        EnforcementLevel lvl = (contract.level != EnforcementLevel::NONE)
            ? contract.level : rules().contracts.level;
        if (lvl == EnforcementLevel::HARD || (lvl == EnforcementLevel::SOFT && !override_enabled_)) {
            return msg;
        }
        fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
        return "";
    }

    // Check each argument type
    for (size_t i = 0; i < contract.params.size(); ++i) {
        const auto& param_spec = contract.params[i];
        // Parse "name:type" format
        auto colon_pos = param_spec.find(':');
        if (colon_pos == std::string::npos) continue;  // No type constraint

        std::string param_name = param_spec.substr(0, colon_pos);
        std::string expected_type = param_spec.substr(colon_pos + 1);

        if (expected_type == "any") continue;  // Accept anything

        if (i < arg_types.size() && arg_types[i] != expected_type) {
            std::string msg = "Contract violation [" + func_name + "]: parameter '" +
                              param_name + "' expected type '" + expected_type +
                              "', got '" + arg_types[i] + "'";
            if (line > 0) msg += " (line " + std::to_string(line) + ")";

            EnforcementLevel lvl = (contract.level != EnforcementLevel::NONE)
                ? contract.level : rules().contracts.level;
            if (lvl == EnforcementLevel::HARD || (lvl == EnforcementLevel::SOFT && !override_enabled_)) {
                return msg;
            }
            fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
        }
    }

    return "";
}

// --- Cosmetic Sanitizer Detection ---
// Instead of listing specific cosmetic transforms (whack-a-mole),
// check if a sanitize_*/validate_* function contains ANY real
// sanitization operation. If none found, it's cosmetic.

std::string GovernanceEngine::checkCosmeticSanitizer(
    const std::string& function_name,
    const std::string& body,
    int line) {

    auto& cfg = rules().code_quality.no_incomplete_logic;
    if (!cfg.enabled) return "";
    clearTrace();

    // Only check functions named sanitize_*/validate_*/check_*/verify_*
    static const std::regex sanitizer_prefix(
        "^(?:sanitize|validate|check|verify)_", std::regex::icase);
    if (!std::regex_search(function_name, sanitizer_prefix)) return "";
    addTrace(fmt::format("'{}' identified as sanitizer/validator (prefix match)", function_name));

    // Skip short functions (< 3 lines can't meaningfully sanitize)
    int line_count = static_cast<int>(std::count(body.begin(), body.end(), '\n')) + 1;
    if (line_count < 3) {
        addTrace(fmt::format("skipped: {} lines < 3 minimum", line_count));
        return "";
    }
    addTrace(fmt::format("body has {} lines", line_count));

    // Strip comments — prevent // regex.match(...) from fooling sanitization detection.
    // Input is already string-stripped (stripStringLiterals at call site line 4247),
    // so stripComments won't be confused by // inside strings.
    std::string body_clean = stripComments(body);

    // Check for real sanitization operations
    bool has_real_sanitization = false;

    // 1. Pattern validation (regex library usage)
    if (body_clean.find("regex.") != std::string::npos ||
        body_clean.find("regex_") != std::string::npos ||
        body_clean.find("re.match") != std::string::npos ||
        body_clean.find("re.search") != std::string::npos) {
        has_real_sanitization = true;
    }

    // 2. Rejection path: if condition → return false/null/0/throw
    if (!has_real_sanitization) {
        static const std::regex rejection(
            "if\\s+[^{]*\\{[^}]*(?:return\\s+(?:false|null|0|\"\"|\\[\\]|\\{\\})|throw\\s)",
            std::regex::icase);
        if (std::regex_search(body_clean, rejection)) has_real_sanitization = true;
    }

    // 3. Type checking with rejection (type() + != or ==)
    if (!has_real_sanitization) {
        if (body_clean.find("type(") != std::string::npos &&
            (body_clean.find("!=") != std::string::npos || body_clean.find("==") != std::string::npos) &&
            (body_clean.find("return") != std::string::npos || body_clean.find("throw") != std::string::npos)) {
            has_real_sanitization = true;
        }
    }

    // 4. Numeric conversion with try/catch (real error handling)
    if (!has_real_sanitization) {
        if ((body_clean.find("int(") != std::string::npos || body_clean.find("float(") != std::string::npos) &&
            body_clean.find("try") != std::string::npos && body_clean.find("catch") != std::string::npos) {
            has_real_sanitization = true;
        }
    }

    // 5. Bounds/range checking
    if (!has_real_sanitization) {
        static const std::regex bounds_check(
            "(?:>=?|<=?|>|<)\\s*(?:\\d+|len\\(|length\\(|size\\()",
            std::regex::icase);
        if (std::regex_search(body_clean, bounds_check) &&
            (body_clean.find("return") != std::string::npos || body_clean.find("throw") != std::string::npos)) {
            has_real_sanitization = true;
        }
    }

    // 6. Allowlist/blocklist check (.contains() or "in [")
    if (!has_real_sanitization) {
        if ((body_clean.find(".contains(") != std::string::npos ||
             body_clean.find(" in [") != std::string::npos ||
             body_clean.find(" not in ") != std::string::npos) &&
            (body_clean.find("return") != std::string::npos || body_clean.find("throw") != std::string::npos)) {
            has_real_sanitization = true;
        }
    }

    if (has_real_sanitization) {
        addTrace("real sanitization found → PASS");
    }
    if (!has_real_sanitization) {
        addTrace("no real sanitization ops found (no regex, no rejection, no type check, no bounds check) → FAIL");
        return enforce("code_quality.no_incomplete_logic", cfg.level,
            formatError(cfg.level,
                fmt::format("Function '{}' is named as a sanitizer/validator but contains "
                    "no real sanitization — no regex, no rejection path, no type check, "
                    "no bounds check", function_name),
                line > 0 ? fmt::format("line {}", line) : "",
                "code_quality.no_incomplete_logic",
                "Sanitizer/validator functions must contain real validation logic:\n"
                "  - Type checking: if type(x) != \"string\" { return null }\n"
                "  - Bounds check: if val < 0 { return 0 }\n"
                "  - Pattern match: if !regex.test(pattern, input) { return null }\n"
                "  - Allowlist: if category not in allowed { throw ... }\n\n"
                "  string(x).trim() or string(x).lower() alone is NOT sanitization.",
                "fn sanitize_input(data) {\n"
                "    let s = string(data)\n"
                "    return s.trim()  // cosmetic — no validation\n"
                "}",
                "fn sanitize_input(data) {\n"
                "    if type(data) != \"string\" { return \"\" }\n"
                "    let s = data.trim()\n"
                "    if s.length() > 1000 { return s.substring(0, 1000) }\n"
                "    return s\n"
                "}"));
    }
    return "";
}

// --- Complexity Floor Check ---

std::string GovernanceEngine::checkComplexityFloor(
    const std::string& code,
    const std::string& function_name,
    int line) {

    auto& cfg = rules().code_quality.complexity_floor;
    clearTrace();

    // B2: Skip floor for short functions (can't meaningfully reach high scores)
    if (cfg.min_lines_for_check > 0) {
        int line_count = static_cast<int>(std::count(code.begin(), code.end(), '\n')) + 1;
        if (line_count < cfg.min_lines_for_check) {
            addTrace(fmt::format("skipped: {} lines < min_lines_for_check={}", line_count, cfg.min_lines_for_check));
            return "";
        }
    }

    // Analyze code structure (defensive — never crash the host program)
    analyzer::SyntacticAnalyzer sa;
    analyzer::SyntacticProfile profile;
    try {
        profile = sa.analyze(code, function_name);
    } catch (const governance::GovernanceHardError&) {
        throw;
    } catch (...) {
        addTrace("skipped: syntactic analysis failed");
        return "";  // Can't analyze — skip floor check
    }

    // Re-score with custom weights if configured (overrides SyntacticAnalyzer defaults)
    if (cfg.weights.custom) {
        const auto& w = cfg.weights;
        int score = 0;
        int real_loops = profile.loop_count - profile.padding_loop_count;
        score += real_loops * w.loop;
        score += profile.padding_loop_count * w.padding_loop;
        if (profile.has_nested_loops) score += w.nested_loops;
        if (profile.has_large_iterations) score += w.large_iterations;
        score += profile.function_count * w.function;
        if (profile.has_recursion) score += w.recursion;
        if (profile.has_array_operations) score += w.array_ops;
        score += std::min(profile.pipeline_count * w.pipeline, w.pipeline_cap);
        if (profile.has_comprehension) score += w.comprehension;
        if (profile.allocates_memory) score += w.memory_alloc;
        if (profile.manages_lifetime) score += w.lifetime;
        if (profile.uses_pointers) score += w.pointers;
        if (profile.has_try_catch) score += w.try_catch;
        if (profile.propagates_errors) score += w.error_propagation;
        score += static_cast<int>(profile.imported_modules.size()) * w.import;
        score += profile.external_call_count * w.external_call;
        profile.complexity_score = std::min(score, 100);
    }

    // Determine which rule applies (if any)
    int required_score = cfg.min_score;
    bool require_branching = false;
    std::string custom_message;

    // Check name-specific rules (first match wins)
    std::string fn_lower = function_name;
    std::transform(fn_lower.begin(), fn_lower.end(), fn_lower.begin(), ::tolower);

    bool rule_matched = false;
    for (const auto& rule : cfg.rules) {
        bool matched = false;
        for (const auto& name : rule.names) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            auto pos = fn_lower.find(name_lower);
            if (pos != std::string::npos) {
                // Word-boundary matching using '_' as delimiter (snake_case)
                bool boundary_before = (pos == 0 || fn_lower[pos - 1] == '_');
                size_t end = pos + name_lower.size();
                bool boundary_after = (end >= fn_lower.size() || fn_lower[end] == '_');
                // Prefix patterns ending in '_' (is_, has_, to_) only need boundary before
                bool is_prefix = !name_lower.empty() && name_lower.back() == '_';
                if (boundary_before && (is_prefix || boundary_after)) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            rule_matched = true;
            required_score = rule.min_score;
            require_branching = rule.require_branching_or_loops;
            custom_message = rule.message;
            addTrace(fmt::format("rule matched: '{}' matched prefix → min_score={}", function_name, required_score));
            break;
        }
    }

    // If rules exist but none matched this function name, skip the floor check.
    // This is the intended behavior of target_prefixes: only check listed prefixes.
    if (!cfg.rules.empty() && !rule_matched) {
        addTrace(fmt::format("skipped: '{}' not matched by any target prefix", function_name));
        return "";
    }

    addTrace(fmt::format("complexity profile: score={}, loops={} (padding={}), nesting={}, calls={}, recursion={}, pipelines={}",
        profile.complexity_score, profile.loop_count, profile.padding_loop_count,
        profile.max_function_depth, profile.external_call_count,
        profile.has_recursion ? "yes" : "no", profile.pipeline_count));
    addTrace(fmt::format("verdict: score={} vs required={} → {}",
        profile.complexity_score, required_score,
        profile.complexity_score >= required_score ? "PASS" : "FAIL"));

    // Check complexity score against floor
    if (profile.complexity_score < required_score) {
        std::string msg = custom_message.empty()
            ? fmt::format("Function '{}' has complexity score {} (minimum: {})",
                function_name, profile.complexity_score, required_score)
            : custom_message;

        std::string padding_warning;
        if (profile.padding_loop_count > 0) {
            padding_warning = fmt::format(
                "\n\n  WARNING: {} small-range loop(s) detected (0..1, 0..2, 0..3).\n"
                "  These only add +1 each (not +5). They do NOT count as real complexity.\n"
                "  Replace with loops over real data: for item in data {{ ... }} adds +5.",
                profile.padding_loop_count);
        }
        return enforce("code_quality.complexity_floor", cfg.level,
            formatError(cfg.level, msg,
                line > 0 ? fmt::format("line {}", line) : "",
                "code_quality.complexity_floor",
                fmt::format("Score {}/100: loops={} (padding={}), nesting={}, calls={}, recursion={}, pipelines={}\n\n"
                    "  What adds complexity:\n"
                    "    +5  each real loop (for/while over data)     +5  try/catch\n"
                    "    +1  small-range loop (0..1, 0..2, 0..3)     +5  array operations\n"
                    "    +15 nested loops                              +3  per |> pipeline stage (cap 15)\n"
                    "    +3  each function definition                  +10 recursion\n"
                    "    +1  each external function call\n\n"
                    "  Tip: Add real logic — input validation, edge cases, error handling.\n"
                    "  Do NOT pad with for i in 0..1 {{}} loops.{}",
                    profile.complexity_score, profile.loop_count, profile.padding_loop_count,
                    (profile.has_try_catch ? 1 : 0) + profile.max_function_depth,
                    profile.external_call_count, profile.has_recursion ? "yes" : "no",
                    profile.pipeline_count, padding_warning),
                "fn apply_damage(ent, damage) {\n"
                "    ent[\"hp\"] = ent.get(\"hp\") - damage\n"
                "    for i in 0..1 { if ent.get(\"hp\") < 0 { ent[\"hp\"] = 0 } }\n"
                "    return ent\n"
                "}",
                "fn apply_damage(ent, damage) {\n"
                "    let max_hp = ent.get(\"max_hp\") ?? 100\n"
                "    let actual = int(math.max(0, damage))\n"
                "    let new_hp = ent.get(\"hp\") - actual\n"
                "    if new_hp <= 0 { ent[\"hp\"] = 0; ent[\"alive\"] = false }\n"
                "    else if new_hp > max_hp { ent[\"hp\"] = max_hp }\n"
                "    else { ent[\"hp\"] = new_hp }\n"
                "    return ent\n"
                "}"));
    }

    // Check branching/loops requirement
    if (require_branching) {
        // max_function_depth > 0 means nested loops exist (valid non-trivial indicator)
        bool has_branching = profile.loop_count > 0 || profile.has_try_catch ||
                            profile.max_function_depth > 0 || profile.has_recursion;
        if (!has_branching) {
            std::string msg = custom_message.empty()
                ? fmt::format("Function '{}' requires branching or loops but has none", function_name)
                : custom_message;

            return enforce("code_quality.complexity_floor", cfg.level,
                formatError(cfg.level, msg,
                    line > 0 ? fmt::format("line {}", line) : "",
                    "code_quality.complexity_floor",
                    "Functions named analyze/compute/process must contain loops,\n"
                    "conditionals, or recursive logic — not just assignments and returns.",
                    "fn compute_score(data) {\n"
                    "    let score = data.get(\"base\") * 1.5\n"
                    "    return score\n"
                    "}",
                    "fn compute_score(data) {\n"
                    "    let score = 0\n"
                    "    for item in data.get(\"items\") {\n"
                    "        let weight = item.get(\"weight\") ?? 1\n"
                    "        if item.get(\"type\") == \"bonus\" {\n"
                    "            score = score + weight * 2\n"
                    "        } else {\n"
                    "            score = score + weight\n"
                    "        }\n"
                    "    }\n"
                    "    return score\n"
                    "}"));
        }
    }

    return "";
}

// --- Behavioral Contract Check (must_call) ---

std::string GovernanceEngine::checkFunctionBehavioralContract(
    const std::string& func_name,
    const std::string& func_body,
    int line,
    int param_count) {

    auto it = rules().contracts.functions.find(func_name);
    if (it == rules().contracts.functions.end()) return "";

    const auto& contract = it->second;

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    // naab-29 L-08: Arity check
    if (param_count >= 0) {
        if (contract.min_arity >= 0 && param_count < contract.min_arity) {
            addTrace(fmt::format("function '{}' has {} params, min required: {}",
                func_name, param_count, contract.min_arity));
            return enforce("contracts." + func_name + ".arity", level,
                formatError(level,
                    fmt::format("Function '{}' has {} parameter(s), minimum required: {}",
                        func_name, param_count, contract.min_arity),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.min_arity",
                    "Function does not meet minimum arity requirement",
                    fmt::format("fn {}(...) // {} params", func_name, param_count),
                    fmt::format("fn {}(...) // at least {} params", func_name, contract.min_arity)));
        }
        if (contract.max_arity >= 0 && param_count > contract.max_arity) {
            addTrace(fmt::format("function '{}' has {} params, max allowed: {}",
                func_name, param_count, contract.max_arity));
            return enforce("contracts." + func_name + ".arity", level,
                formatError(level,
                    fmt::format("Function '{}' has {} parameter(s), maximum allowed: {}",
                        func_name, param_count, contract.max_arity),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.max_arity",
                    "Function exceeds maximum arity requirement",
                    fmt::format("fn {}(...) // {} params", func_name, param_count),
                    fmt::format("fn {}(...) // at most {} params", func_name, contract.max_arity)));
        }
    }

    if (contract.must_call.empty() && contract.must_contain.empty()
        && contract.must_derive_from.empty()) return "";

    for (const auto& required : contract.must_call) {
        // Check if the required pattern appears in the function body.
        // For simple identifiers, use word boundary + opening paren.
        // For patterns like "<<python", check literal presence.
        bool found = false;
        if (required.find("<<") != std::string::npos) {
            // Polyglot block pattern — literal string search
            found = func_body.find(required) != std::string::npos;
        } else {
            // Function call pattern — word boundary + paren
            try {
                std::regex call_pat("\\b" + required + "\\s*\\(");
                found = std::regex_search(func_body, call_pat);
            } catch (...) {
                // Invalid regex — fall back to literal search
                found = func_body.find(required) != std::string::npos;
            }
        }

        if (!found) {
            addTrace(fmt::format("function '{}' must call '{}' but does not", func_name, required));
            return enforce("contracts." + func_name + ".must_call", level,
                formatError(level,
                    fmt::format("Behavioral contract: '{}' must call '{}'", func_name, required),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_call",
                    fmt::format("Function '{}' is required to call '{}' but no such call was found in the function body.",
                        func_name, required),
                    "fn " + func_name + "(...) { return {\"key\": \"value\"} }",
                    "fn " + func_name + "(...) { let r = " + required + "(...); return r }"));
        }
    }

    // must_contain: literal string search for operators, keywords, syntax patterns
    for (const auto& required : contract.must_contain) {
        if (func_body.find(required) == std::string::npos) {
            addTrace(fmt::format("function '{}' must contain '{}' but does not", func_name, required));
            return enforce("contracts." + func_name + ".must_contain", level,
                formatError(level,
                    fmt::format("Behavioral contract: '{}' must contain '{}'", func_name, required),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_contain",
                    fmt::format("Function '{}' is required to contain '{}' but the pattern was not found in the function body.",
                        func_name, required),
                    "fn " + func_name + "(...) { ... }",
                    "fn " + func_name + "(...) { ... " + required + " ... }"));
        }
    }

    // must_derive_from: check that function body references specified parameters
    // Strip the function signature — only search inside the body braces.
    // func_body is the full function text: "fn name(params) { ... }"
    std::string inner_body;
    {
        auto brace_pos = func_body.find('{');
        if (brace_pos != std::string::npos) {
            inner_body = func_body.substr(brace_pos + 1);
            // Remove trailing closing brace
            auto last_brace = inner_body.rfind('}');
            if (last_brace != std::string::npos) {
                inner_body = inner_body.substr(0, last_brace);
            }
        } else {
            inner_body = func_body;
        }
    }
    for (const auto& spec : contract.must_derive_from) {
        bool param_found = false;
        for (const auto& param : spec.params) {
            try {
                std::regex param_pat("\\b" + param + "\\b");
                if (std::regex_search(inner_body, param_pat)) {
                    param_found = true;
                    break;
                }
            } catch (...) {
                // Invalid regex — fall back to literal search
                if (inner_body.find(param) != std::string::npos) {
                    param_found = true;
                    break;
                }
            }
        }
        if (!param_found) {
            std::string params_list;
            for (size_t pi = 0; pi < spec.params.size(); ++pi) {
                if (pi > 0) params_list += ", ";
                params_list += spec.params[pi];
            }
            addTrace(fmt::format("function '{}' return key '{}' must derive from [{}] but none referenced",
                func_name, spec.return_key, params_list));
            return enforce("contracts." + func_name + ".must_derive_from", level,
                formatError(level,
                    fmt::format("Derivation contract: '{}' key '{}' must use parameter '{}'",
                        func_name, spec.return_key, spec.params[0]),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_derive_from",
                    fmt::format("Function '{}' return key '{}' must be computed from parameter(s) [{}] "
                        "but none of those parameters appear in the function body.",
                        func_name, spec.return_key, params_list),
                    "fn " + func_name + "(...) { return {\"" + spec.return_key + "\": 0.1} }",
                    "fn " + func_name + "(" + spec.params[0] + ") { return {\"" + spec.return_key
                        + "\": compute(" + spec.params[0] + ")} }"));
        }
    }

    recordPass("contracts." + func_name + ".behavioral", level);
    return "";
}

// --- Execution-Based Contract Infrastructure (v6) ---

interpreter::NaabVal GovernanceEngine::jsonStringToNaabVal(const std::string& json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (j.is_null()) return interpreter::NaabVal::makeNull();
        if (j.is_boolean()) return interpreter::NaabVal::makeBool(j.get<bool>());
        if (j.is_number_integer()) return interpreter::NaabVal::makeInt(j.get<int>());
        if (j.is_number_float()) return interpreter::NaabVal::makeDouble(j.get<double>());
        if (j.is_string()) return interpreter::NaabVal::makeString(j.get<std::string>());
        if (j.is_array()) {
            std::vector<interpreter::NaabVal> items;
            for (auto& el : j) items.push_back(jsonStringToNaabVal(el.dump()));
            return interpreter::NaabVal::makeList(std::move(items));
        }
        if (j.is_object()) {
            std::unordered_map<std::string, interpreter::NaabVal> map;
            for (auto& [k, v] : j.items()) map[k] = jsonStringToNaabVal(v.dump());
            return interpreter::NaabVal::makeDict(std::move(map));
        }
    } catch (...) {}
    return interpreter::NaabVal::makeNull();
}

interpreter::NaabVal GovernanceEngine::callContractTestFunction(
    const std::string& func_name,
    const std::vector<interpreter::NaabVal>& args) {

    // --- VM path (via type-erased callbacks to avoid link dependency) ---
    if (vm_call_fn_ && vm_globals_fn_) {
        const auto& globals = vm_globals_fn_();
        interpreter::NaabVal fn;

        // Phase 1: bare name lookup
        auto it = globals.find(func_name);
        if (it != globals.end()) {
            fn = it->second;
        }
        // Phase 2: search module dicts
        if (fn.isNull()) {
            for (const auto& [name, val] : globals) {
                if (val.isDict()) {
                    const auto& dict = val.asDictConst();
                    auto dit = dict.find(func_name);
                    if (dit != dict.end() && (dit->second.isFunction() || dit->second.isVMClosure())) {
                        fn = dit->second;
                        break;
                    }
                }
            }
        }
        // Phase 3: dotted name lookup
        if (fn.isNull()) {
            std::string suffix = "." + func_name;
            for (const auto& [name, val] : globals) {
                if (name.size() > suffix.size() &&
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                    (val.isFunction() || val.isVMClosure())) {
                    fn = val;
                    break;
                }
            }
        }
        if (fn.isNull()) {
            throw std::runtime_error(
                fmt::format("Contract test: function '{}' not found in global scope", func_name));
        }
        // Pad args to match function arity (contracts may supply fewer args
        // than the function requires — e.g., must_vary supplies 1 arg for a 2-param fn)
        auto padded_args = args;
        if (fn.isVMClosure()) {
            int arity = fn.asVMClosureConst()->function->arity;
            while (static_cast<int>(padded_args.size()) < arity) {
                padded_args.push_back(interpreter::NaabVal::makeList({}));  // empty list
            }
        }
        return vm_call_fn_(fn, padded_args, false);  // contract test args — not tainted
    }

    // --- Tree-walker path ---
    auto* interp = interpreter::g_current_interpreter;
    if (!interp) {
        throw std::runtime_error(
            "Execution-based contracts require an active execution engine");
    }

    interpreter::NaabVal fn;
    auto env = interp->getGlobalEnv();
    // Try bare name first (global functions)
    if (env->has(func_name)) {
        fn = env->get(func_name);
    } else {
        // Search module dicts: wildcard imports store a dict under the alias,
        // e.g. env["helper"] = {"normalize_severity": <fn>, ...}
        for (const auto& [name, val] : env->getValues()) {
            if (val.isDict()) {
                const auto& dict = val.asDictConst();
                auto it = dict.find(func_name);
                if (it != dict.end() && (it->second.isFunction() || it->second.isVMClosure())) {
                    fn = it->second;
                    break;
                }
            }
        }
        // Also try module-qualified names: "alias.func" defined directly in global env
        if (fn.isNull()) {
            std::string suffix = "." + func_name;
            for (const auto& [name, val] : env->getValues()) {
                if (name.size() > suffix.size() &&
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                    (val.isFunction() || val.isVMClosure())) {
                    fn = val;
                    break;
                }
            }
        }
        if (fn.isNull()) {
            throw std::runtime_error(
                fmt::format("Contract test: function '{}' not found in global scope", func_name));
        }
    }
    if (fn.isNull()) {
        throw std::runtime_error(
            fmt::format("Contract test: function '{}' is null", func_name));
    }

    // Pad args to match function arity (same as VM path above)
    auto padded_args = args;
    if (fn.isFunction()) {
        size_t arity = fn.asFunctionConst()->params.size();
        while (padded_args.size() < arity) {
            padded_args.push_back(interpreter::NaabVal::makeList({}));  // empty list
        }
    } else if (fn.isVMClosure()) {
        int arity = fn.asVMClosureConst()->function->arity;
        while (static_cast<int>(padded_args.size()) < arity) {
            padded_args.push_back(interpreter::NaabVal::makeList({}));  // empty list
        }
    }

    return interp->callFunction(fn, padded_args);
}

std::string GovernanceEngine::checkMustProduce(
    const std::string& func_name,
    const FunctionContract& contract,
    int line) {

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    for (size_t i = 0; i < contract.must_produce.size(); ++i) {
        const auto& tc = contract.must_produce[i];

        // Build args from JSON strings
        std::vector<interpreter::NaabVal> args;
        for (const auto& arg_json : tc.args_json) {
            args.push_back(jsonStringToNaabVal(arg_json));
        }

        // Build display string for error messages
        std::string args_display = "[";
        for (size_t a = 0; a < tc.args_json.size(); ++a) {
            if (a > 0) args_display += ", ";
            args_display += tc.args_json[a];
        }
        args_display += "]";

        // Call function with re-entrancy guard
        interpreter::NaabVal result;
        try {
            in_contract_test_ = true;
            result = callContractTestFunction(func_name, args);
            in_contract_test_ = false;
        } catch (const governance::GovernanceHardError&) {
            in_contract_test_ = false;
            throw;  // uncatchable — propagate to main
        } catch (const std::exception& e) {
            in_contract_test_ = false;
            addTrace(fmt::format("must_produce test #{}: call threw: {}", i + 1, e.what()));
            return enforce("contracts." + func_name + ".must_produce", level,
                formatError(level,
                    fmt::format("must_produce test #{} for '{}' threw an exception", i + 1, func_name),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_produce",
                    fmt::format("Function threw: {}", e.what()),
                    fmt::format("{}({}) => error", func_name, args_display),
                    fmt::format("{}({}) => {}", func_name, args_display, tc.expect_json)));
        }

        // Compare result to expected — naab-29 L-07: type-strict comparison
        interpreter::NaabVal expected = jsonStringToNaabVal(tc.expect_json);

        // Check type match first: "0" (string) != 0 (int)
        bool type_mismatch = false;
        if (result.isString() != expected.isString() ||
            result.isInt() != expected.isInt() ||
            result.isBool() != expected.isBool() ||
            result.isDouble() != expected.isDouble()) {
            type_mismatch = true;
        }

        std::string result_str = result.isNull() ? "null" : result.toString();
        std::string expect_str = expected.isNull() ? "null" : expected.toString();

        if (type_mismatch || result_str != expect_str) {
            addTrace(fmt::format("must_produce test #{}: args={}, expected={}, got={}",
                i + 1, args_display, expect_str, result_str));
            return enforce("contracts." + func_name + ".must_produce", level,
                formatError(level,
                    fmt::format("must_produce: '{}' returned wrong value", func_name),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_produce",
                    fmt::format("Input: {}\n  Expected: {}\n  Got: {}",
                        args_display, expect_str, result_str),
                    fmt::format("{}({}) => {}", func_name, args_display, result_str),
                    fmt::format("{}({}) => {}", func_name, args_display, expect_str)));
        }
    }

    recordPass("contracts." + func_name + ".must_produce", level);
    return "";
}

std::string GovernanceEngine::checkMustVary(
    const std::string& func_name,
    const FunctionContract& contract,
    int line) {

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    for (size_t i = 0; i < contract.must_vary.size(); ++i) {
        const auto& spec = contract.must_vary[i];

        // Build two distinct input sets
        std::vector<interpreter::NaabVal> args_a, args_b;
        if (spec.fixtures_json.size() >= 2) {
            // User provided fixtures — use first two
            args_a.push_back(jsonStringToNaabVal(spec.fixtures_json[0]));
            args_b.push_back(jsonStringToNaabVal(spec.fixtures_json[1]));
        } else {
            // Generate synthetic inputs: empty list vs populated list
            // Include common domain fields so functions that access .get("state"),
            // .get("source"), etc. don't crash on missing keys
            args_a.push_back(interpreter::NaabVal::makeList({}));
            std::vector<interpreter::NaabVal> items;
            std::unordered_map<std::string, interpreter::NaabVal> d1, d2, d3;
            d1["severity"] = interpreter::NaabVal::makeInt(0);
            d1["state"] = interpreter::NaabVal::makeInt(0);
            d1["id"] = interpreter::NaabVal::makeString("synth-1");
            d1["source"] = interpreter::NaabVal::makeString("synthetic");
            d1["message"] = interpreter::NaabVal::makeString("test event 1");
            d1["timestamp"] = interpreter::NaabVal::makeInt(1000000);
            d2["severity"] = interpreter::NaabVal::makeInt(5);
            d2["state"] = interpreter::NaabVal::makeInt(1);  // Firing — triggers state==1 checks
            d2["id"] = interpreter::NaabVal::makeString("synth-2");
            d2["source"] = interpreter::NaabVal::makeString("synthetic");
            d2["message"] = interpreter::NaabVal::makeString("test event 2");
            d2["timestamp"] = interpreter::NaabVal::makeInt(1000060);
            d3["severity"] = interpreter::NaabVal::makeInt(10);
            d3["state"] = interpreter::NaabVal::makeInt(2);  // Acknowledged — triggers state>=2 checks
            d3["id"] = interpreter::NaabVal::makeString("synth-3");
            d3["source"] = interpreter::NaabVal::makeString("synthetic");
            d3["message"] = interpreter::NaabVal::makeString("test event 3");
            d3["timestamp"] = interpreter::NaabVal::makeInt(1000120);
            items.push_back(interpreter::NaabVal::makeDict(std::move(d1)));
            items.push_back(interpreter::NaabVal::makeDict(std::move(d2)));
            items.push_back(interpreter::NaabVal::makeDict(std::move(d3)));
            args_b.push_back(interpreter::NaabVal::makeList(std::move(items)));
        }

        // Call function with both inputs
        interpreter::NaabVal result_a, result_b;
        try {
            in_contract_test_ = true;
            result_a = callContractTestFunction(func_name, args_a);
            result_b = callContractTestFunction(func_name, args_b);
            in_contract_test_ = false;
        } catch (const governance::GovernanceHardError&) {
            in_contract_test_ = false;
            throw;  // uncatchable — propagate to main
        } catch (const std::exception& e) {
            in_contract_test_ = false;
            addTrace(fmt::format("must_vary test #{}: call threw: {}", i + 1, e.what()));
            return enforce("contracts." + func_name + ".must_vary", level,
                formatError(level,
                    fmt::format("must_vary test #{} for '{}' threw an exception", i + 1, func_name),
                    "", "contracts.must_vary",
                    fmt::format("Function threw: {}", e.what()), "", ""));
        }

        // Extract the specified key from both results
        std::string val_a, val_b;
        if (result_a.isDict()) {
            auto& dict = result_a.asDict();
            auto it = dict.find(spec.key);
            val_a = (it != dict.end()) ? it->second.toString() : "<missing>";
        } else {
            val_a = result_a.toString();
        }
        if (result_b.isDict()) {
            auto& dict = result_b.asDict();
            auto it = dict.find(spec.key);
            val_b = (it != dict.end()) ? it->second.toString() : "<missing>";
        } else {
            val_b = result_b.toString();
        }

        if (val_a == val_b) {
            addTrace(fmt::format("must_vary: '{}' key '{}' returned '{}' for both inputs — hardcoded",
                func_name, spec.key, val_a));
            return enforce("contracts." + func_name + ".must_vary", level,
                formatError(level,
                    fmt::format("must_vary: '{}' key '{}' is hardcoded", func_name, spec.key),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_vary",
                    fmt::format("Key '{}' returned the same value '{}' for two different '{}' inputs.\n"
                        "  The value appears to be hardcoded rather than computed from the input.",
                        spec.key, val_a, spec.across),
                    fmt::format("{}(...) => {{\"{}\": {}}} (always)", func_name, spec.key, val_a),
                    fmt::format("{}(...) => {{\"{}\": <computed from {}>}}", func_name, spec.key, spec.across)));
        }
    }

    recordPass("contracts." + func_name + ".must_vary", level);
    return "";
}

std::string GovernanceEngine::checkMustDifferentiate(
    const std::string& func_name,
    const FunctionContract& contract,
    int line) {

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    for (size_t i = 0; i < contract.must_differentiate.size(); ++i) {
        const auto& dc = contract.must_differentiate[i];

        // Build args from the two inputs
        std::vector<interpreter::NaabVal> args_a = {jsonStringToNaabVal(dc.input_a_json)};
        std::vector<interpreter::NaabVal> args_b = {jsonStringToNaabVal(dc.input_b_json)};

        // Call function with both inputs
        interpreter::NaabVal result_a, result_b;
        try {
            in_contract_test_ = true;
            result_a = callContractTestFunction(func_name, args_a);
            result_b = callContractTestFunction(func_name, args_b);
            in_contract_test_ = false;
        } catch (const governance::GovernanceHardError&) {
            in_contract_test_ = false;
            throw;  // uncatchable — propagate to main
        } catch (const std::exception& e) {
            in_contract_test_ = false;
            addTrace(fmt::format("must_differentiate test #{}: call threw: {}", i + 1, e.what()));
            return enforce("contracts." + func_name + ".must_differentiate", level,
                formatError(level,
                    fmt::format("must_differentiate test #{} for '{}' threw an exception", i + 1, func_name),
                    "", "contracts.must_differentiate",
                    fmt::format("Function threw: {}", e.what()), "", ""));
        }

        // Extract the specified key from both results
        std::string val_a, val_b;
        if (result_a.isDict()) {
            auto& dict = result_a.asDict();
            auto it = dict.find(dc.key);
            val_a = (it != dict.end()) ? it->second.toString() : "<missing>";
        } else {
            val_a = result_a.toString();
        }
        if (result_b.isDict()) {
            auto& dict = result_b.asDict();
            auto it = dict.find(dc.key);
            val_b = (it != dict.end()) ? it->second.toString() : "<missing>";
        } else {
            val_b = result_b.toString();
        }

        if (val_a == val_b) {
            addTrace(fmt::format("must_differentiate: '{}' key '{}' returned '{}' for both inputs",
                func_name, dc.key, val_a));
            return enforce("contracts." + func_name + ".must_differentiate", level,
                formatError(level,
                    fmt::format("must_differentiate: '{}' does not distinguish inputs on key '{}'",
                        func_name, dc.key),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_differentiate",
                    fmt::format("Input A: {}\n  Input B: {}\n  Both returned '{}' = '{}'\n"
                        "  The function must produce different '{}' values for these distinct inputs.",
                        dc.input_a_json, dc.input_b_json, dc.key, val_a, dc.key),
                    fmt::format("{}(A) => {{\"{}\": \"{}\"}}, {}(B) => {{\"{}\": \"{}\"}}",
                        func_name, dc.key, val_a, func_name, dc.key, val_a),
                    fmt::format("{}(A) => {{\"{}\": \"<class_a>\"}}, {}(B) => {{\"{}\": \"<class_b>\"}}",
                        func_name, dc.key, func_name, dc.key)));
        }
    }

    recordPass("contracts." + func_name + ".must_differentiate", level);
    return "";
}

std::string GovernanceEngine::checkMustHandleCase(
    const std::string& func_name,
    const FunctionContract& contract,
    int line) {

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    for (size_t i = 0; i < contract.must_handle_case.size(); ++i) {
        const auto& spec = contract.must_handle_case[i];
        if (spec.inputs_json.size() < 2) continue;

        // Call function with each case-varied input, collect results
        std::vector<std::string> results;
        try {
            in_contract_test_ = true;
            for (const auto& inp_json : spec.inputs_json) {
                std::vector<interpreter::NaabVal> args = {jsonStringToNaabVal(inp_json)};
                auto result = callContractTestFunction(func_name, args);
                results.push_back(result.isNull() ? "null" : result.toString());
            }
            in_contract_test_ = false;
        } catch (const governance::GovernanceHardError&) {
            in_contract_test_ = false;
            throw;  // uncatchable — propagate to main
        } catch (const std::exception& e) {
            in_contract_test_ = false;
            addTrace(fmt::format("must_handle_case test #{}: call threw: {}", i + 1, e.what()));
            return enforce("contracts." + func_name + ".must_handle_case", level,
                formatError(level,
                    fmt::format("must_handle_case test #{} for '{}' threw an exception", i + 1, func_name),
                    "", "contracts.must_handle_case",
                    fmt::format("Function threw: {}", e.what()), "", ""));
        }

        // Check all results are the same (consistent) or different
        bool all_same = true;
        for (size_t r = 1; r < results.size(); ++r) {
            if (results[r] != results[0]) { all_same = false; break; }
        }

        if (spec.expect == "consistent" && !all_same) {
            // Build input display
            std::string inputs_display;
            for (size_t j = 0; j < spec.inputs_json.size(); ++j) {
                if (j > 0) inputs_display += ", ";
                inputs_display += spec.inputs_json[j] + " => " + results[j];
            }
            addTrace(fmt::format("must_handle_case: '{}' returned inconsistent results for case variants",
                func_name));
            return enforce("contracts." + func_name + ".must_handle_case", level,
                formatError(level,
                    fmt::format("must_handle_case: '{}' is case-sensitive (expected consistent)", func_name),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_handle_case",
                    fmt::format("Case-varied inputs produced different results:\n  {}",
                        inputs_display),
                    fmt::format("{}({}) => {}", func_name, spec.inputs_json[0], results[0]),
                    fmt::format("{}(any_case) => {} (consistent)", func_name, results[0])));
        } else if (spec.expect == "different" && all_same) {
            addTrace(fmt::format("must_handle_case: '{}' returned same result '{}' for all case variants",
                func_name, results[0]));
            return enforce("contracts." + func_name + ".must_handle_case", level,
                formatError(level,
                    fmt::format("must_handle_case: '{}' ignores case (expected different)", func_name),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_handle_case",
                    fmt::format("All case-varied inputs returned the same result: '{}'", results[0]),
                    fmt::format("{}(any_case) => {}", func_name, results[0]),
                    fmt::format("{}(upper) => A, {}(lower) => B", func_name, func_name)));
        }
    }

    recordPass("contracts." + func_name + ".must_handle_case", level);
    return "";
}

std::string GovernanceEngine::checkMustSatisfy(
    const std::string& func_name,
    const FunctionContract& contract,
    int line) {

    clearTrace();
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules().contracts.level;

    // must_satisfy needs test inputs. Priority: must_satisfy_args > must_produce args > no args.
    // must_satisfy_args entries are {"args": [...]} objects — unwrap the args array.
    std::vector<interpreter::NaabVal> test_args;
    if (!contract.must_satisfy_args_json.empty()) {
        // Use first test case. Each entry is JSON: {"args": [arg1, arg2, ...]}
        auto test_case = nlohmann::json::parse(contract.must_satisfy_args_json[0]);
        if (test_case.contains("args") && test_case["args"].is_array()) {
            for (auto& arg : test_case["args"]) {
                test_args.push_back(jsonStringToNaabVal(arg.dump()));
            }
        } else {
            // Fallback: treat each must_satisfy_args entry as a direct arg value
            for (const auto& arg_json : contract.must_satisfy_args_json) {
                test_args.push_back(jsonStringToNaabVal(arg_json));
            }
        }
    } else if (!contract.must_produce.empty() && !contract.must_produce[0].args_json.empty()) {
        for (const auto& arg_json : contract.must_produce[0].args_json) {
            test_args.push_back(jsonStringToNaabVal(arg_json));
        }
    }

    // Call function to get a result to check invariants against
    interpreter::NaabVal result;
    try {
        in_contract_test_ = true;
        result = callContractTestFunction(func_name, test_args);
        in_contract_test_ = false;
    } catch (const governance::GovernanceHardError&) {
        in_contract_test_ = false;
        throw;  // uncatchable — propagate to main
    } catch (const std::exception& e) {
        in_contract_test_ = false;
        addTrace(fmt::format("must_satisfy: call to '{}' threw: {}", func_name, e.what()));
        return enforce("contracts." + func_name + ".must_satisfy", level,
            formatError(level,
                fmt::format("must_satisfy: '{}' threw an exception", func_name),
                "", "contracts.must_satisfy",
                fmt::format("Function threw: {}", e.what()), "", ""));
    }

    // Evaluate each invariant expression: "result.key op value"
    // Regex: result\.(\w+(?:\.\w+)*)\s*(>=|<=|>|<|==|!=)\s*(.+)
    std::regex expr_pat(R"(result\.(\w+(?:\.\w+)*)\s*(>=|<=|>|<|==|!=)\s*(.+))");

    for (const auto& expr : contract.must_satisfy) {
        std::smatch m;
        if (!std::regex_match(expr, m, expr_pat)) {
            addTrace(fmt::format("must_satisfy: unparseable expression: '{}'", expr));
            continue;  // Skip unparseable expressions
        }

        std::string key_path = m[1].str();
        std::string op = m[2].str();
        std::string rhs_str = m[3].str();
        // Trim rhs
        while (!rhs_str.empty() && (rhs_str.back() == ' ' || rhs_str.back() == '\t'))
            rhs_str.pop_back();

        // Extract value at key_path from result
        double lhs_val = 0.0;
        bool found = false;
        if (result.isDict()) {
            auto& dict = result.asDict();
            // Support single-level key for now (e.g., "compliance_rate")
            auto it = dict.find(key_path);
            if (it != dict.end()) {
                found = true;
                if (it->second.isInt()) lhs_val = static_cast<double>(it->second.asInt());
                else if (it->second.isDouble()) lhs_val = it->second.asDouble();
                else {
                    addTrace(fmt::format("must_satisfy: result.{} is not numeric ('{}')",
                        key_path, it->second.toString()));
                    return enforce("contracts." + func_name + ".must_satisfy", level,
                        formatError(level,
                            fmt::format("must_satisfy: result.{} is not numeric", key_path),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "contracts.must_satisfy",
                            fmt::format("Expression '{}': result.{} = '{}' (not a number)",
                                expr, key_path, it->second.toString()),
                            "", ""));
                }
            }
        }
        if (!found) {
            addTrace(fmt::format("must_satisfy: result.{} not found in return value", key_path));
            return enforce("contracts." + func_name + ".must_satisfy", level,
                formatError(level,
                    fmt::format("must_satisfy: result.{} not found", key_path),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_satisfy",
                    fmt::format("Expression '{}': key '{}' not found in return dict",
                        expr, key_path),
                    fmt::format("{}(...) => {}", func_name, result.toString()),
                    fmt::format("{}(...) => {{\"{}\":  <value>}}", func_name, key_path)));
        }

        // Parse rhs as double
        double rhs_val = 0.0;
        try { rhs_val = std::stod(rhs_str); }
        catch (...) {
            addTrace(fmt::format("must_satisfy: cannot parse RHS '{}' as number", rhs_str));
            continue;
        }

        // Evaluate comparison
        bool satisfied = false;
        if (op == ">=") satisfied = lhs_val >= rhs_val;
        else if (op == "<=") satisfied = lhs_val <= rhs_val;
        else if (op == ">") satisfied = lhs_val > rhs_val;
        else if (op == "<") satisfied = lhs_val < rhs_val;
        else if (op == "==") satisfied = lhs_val == rhs_val;
        else if (op == "!=") satisfied = lhs_val != rhs_val;

        if (!satisfied) {
            addTrace(fmt::format("must_satisfy: '{}' FAILED — result.{} = {}, expected {} {}",
                expr, key_path, lhs_val, op, rhs_val));
            return enforce("contracts." + func_name + ".must_satisfy", level,
                formatError(level,
                    fmt::format("must_satisfy: invariant '{}' violated", expr),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "contracts.must_satisfy",
                    fmt::format("result.{} = {}, but expected {} {}",
                        key_path, lhs_val, op, rhs_val),
                    fmt::format("{}(...) => {{\"{}\": {}}}", func_name, key_path, lhs_val),
                    fmt::format("{}(...) => {{\"{}\": <val where val {} {}>}}",
                        func_name, key_path, op, rhs_val)));
        }
    }

    recordPass("contracts." + func_name + ".must_satisfy", level);
    return "";
}

std::string GovernanceEngine::runExecutionContracts() {
    if (!isActive()) return "";
    if (in_contract_test_) return "";  // Re-entrancy guard

    for (const auto& [func_name, contract] : rules().contracts.functions) {
        if (!contract.must_produce.empty()) {
            std::string err = checkMustProduce(func_name, contract, 0);
            if (!err.empty()) return err;
        }
        if (!contract.must_vary.empty()) {
            std::string err = checkMustVary(func_name, contract, 0);
            if (!err.empty()) return err;
        }
        if (!contract.must_differentiate.empty()) {
            std::string err = checkMustDifferentiate(func_name, contract, 0);
            if (!err.empty()) return err;
        }
        if (!contract.must_handle_case.empty()) {
            std::string err = checkMustHandleCase(func_name, contract, 0);
            if (!err.empty()) return err;
        }
        if (!contract.must_satisfy.empty()) {
            std::string err = checkMustSatisfy(func_name, contract, 0);
            if (!err.empty()) return err;
        }
    }

    return "";
}

// --- NAAb Function Body Quality Check ---

std::string GovernanceEngine::checkNaabFunctionBody(
    const std::string& function_name,
    const std::string& source_code,
    int line,
    const std::string& source_file,
    int param_count) {

    setCheckContext(source_file, line);

    auto& os_cfg = rules().code_quality.no_oversimplification;
    auto& il_cfg = rules().code_quality.no_incomplete_logic;
    auto& ph_cfg = rules().code_quality.no_placeholders;

    // Skip if all checks disabled (but still allow complexity_floor, secrets, PII, and plugins if enabled)
    auto& cf_cfg_early = rules().code_quality.complexity_floor;
    bool has_plugins = !rules().governance_plugins.empty();
    bool has_secrets = rules().code_quality.no_secrets.enabled;
    bool has_pii = rules().code_quality.no_pii.enabled;
    if (!os_cfg.enabled && !il_cfg.enabled && !ph_cfg.enabled && !cf_cfg_early.enabled && !has_plugins && !has_secrets && !has_pii) return "";

    // EVA-10: Pre-strip strings to prevent false positives from string literals
    // e.g. a legitimate string "TODO" shouldn't trigger checkPlaceholders
    std::string stripped = stripStringLiterals(source_code);

    // Run applicable checks on the stripped source code
    std::string err;

    if (ph_cfg.enabled) {
        err = checkPlaceholders(stripped, line);
        if (!err.empty()) return err;
    }

    if (os_cfg.enabled) {
        err = checkOversimplification(stripped, line);
        if (!err.empty()) return err;
    }

    if (il_cfg.enabled) {
        err = checkIncompleteLogic(stripped, line, source_file);
        if (!err.empty()) return err;

        // Cosmetic sanitizer check — uses inverted logic: checks for PRESENCE
        // of real sanitization ops rather than listing specific cosmetic patterns
        err = checkCosmeticSanitizer(function_name, stripped, line);
        if (!err.empty()) return err;
    }

    // Secrets/PII checks — use RAW source (secrets CAN be in string literals)
    if (rules().code_quality.no_secrets.enabled) {
        err = checkSecrets(source_code, line);
        if (!err.empty()) return err;
    }
    if (rules().code_quality.no_pii.enabled) {
        err = checkPii(source_code, line);
        if (!err.empty()) return err;
    }

    // Complexity floor check for NAAb functions
    auto& cf_cfg = rules().code_quality.complexity_floor;
    if (cf_cfg.enabled && cf_cfg.check_naab) {
        // Skip if function contains polyglot blocks and skip_if_has_polyglot_block is set
        bool has_polyglot = source_code.find("<<") != std::string::npos;
        if (!has_polyglot || !cf_cfg.skip_if_has_polyglot_block) {
            err = checkComplexityFloor(source_code, function_name, line);
            if (!err.empty()) return err;
        }
    }

    // Behavioral contract check (must_call, arity)
    err = checkFunctionBehavioralContract(function_name, source_code, line, param_count);
    if (!err.empty()) return err;

    // FIX-DX-6: Detect duplicate function calls — deferred to grouped output
    {
        auto& dc_cfg = rules().code_quality.duplicate_calls;
        if (dc_cfg.enabled) {
            std::unordered_map<std::string, int> call_counts;
            size_t ci = 0;
            while (ci < stripped.size()) {
                if (std::isalpha(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_') {
                    std::string word1;
                    while (ci < stripped.size() &&
                           (std::isalnum(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_'))
                        word1 += stripped[ci++];
                    if (ci < stripped.size() && stripped[ci] == '.') {
                        ci++;
                        std::string word2;
                        while (ci < stripped.size() &&
                               (std::isalnum(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_'))
                            word2 += stripped[ci++];
                        if (ci < stripped.size() && stripped[ci] == '(') {
                            call_counts[word1 + "." + word2 + "("]++;
                        }
                    }
                } else {
                    ci++;
                }
            }
            for (const auto& [call, count] : call_counts) {
                if (count >= dc_cfg.threshold) {
                    dup_call_summary_[call].push_back({function_name, count, line});
                }
            }
        }
    }

    // FIX-DX-7: Polyglot blocks without try/catch — deferred to grouped output
    // Keep il_cfg.enabled check for backward compatibility
    {
        auto& ptc_cfg = rules().code_quality.polyglot_try_catch;
        if (il_cfg.enabled && ptc_cfg.enabled) {
            bool has_polyglot_block = source_code.find("<<") != std::string::npos;
            bool has_try_block = source_code.find("try") != std::string::npos;
            if (has_polyglot_block && !has_try_block) {
                ptc_functions_.push_back({function_name, line});
            }
        }
    }

    // Plugin rules (NAAb-based governance checks)
    err = checkPluginRules("naab_function", {
        {"function_name", interpreter::NaabVal::makeString(function_name)},
        {"source_code", interpreter::NaabVal::makeString(source_code)},
        {"source_file", interpreter::NaabVal::makeString(source_file)},
        {"line", interpreter::NaabVal::makeInt(line)},
    }, line);
    if (!err.empty()) return err;

    return "";
}

// --- Hallucinated APIs ---

// Python patterns: JS/Java methods used in Python, made-up Python functions
static const std::vector<std::pair<std::string, std::string>> PYTHON_HALLUCINATION_PATTERNS = {
    // JS methods in Python
    {"\\.length\\b", ".length is JavaScript — in Python, use len()"},
    {"\\.push\\(", ".push() is JavaScript — in Python, use .append()"},
    {"\\.forEach\\(", ".forEach() is JavaScript — in Python, use a for loop"},
    {"\\.indexOf\\(", ".indexOf() is JavaScript — in Python, use .index() or 'in'"},
    {"\\.includes\\(", ".includes() is JavaScript — in Python, use 'in' operator"},
    {"\\.toString\\(", ".toString() is JavaScript — in Python, use str()"},
    {"\\.toUpperCase\\(", ".toUpperCase() is JavaScript — in Python, use .upper()"},
    {"\\.toLowerCase\\(", ".toLowerCase() is JavaScript — in Python, use .lower()"},
    {"\\.trim\\(", ".trim() is JavaScript — in Python, use .strip()"},
    {"\\.charAt\\(", ".charAt() is JavaScript — in Python, use indexing []"},
    {"\\.substring\\(", ".substring() is JavaScript — in Python, use slicing [:]"},
    {"\\.splice\\(", ".splice() is JavaScript — in Python, use slicing or del"},
    {"\\.concat\\(", ".concat() is JavaScript — in Python, use + or .extend()"},
    {"console\\.log\\(", "console.log() is JavaScript — in Python, use print()"},
    {"\\btypeof\\s+", "typeof is JavaScript — in Python, use type()"},
    {"\\binstanceof\\b", "instanceof is JavaScript — in Python, use isinstance()"},
    {"===", "=== is JavaScript — in Python, use =="},
    {"!==", "!== is JavaScript — in Python, use !="},
    {"\\bnull\\b", "null is JavaScript — in Python, use None"},
    {"\\bundefined\\b", "undefined is JavaScript — Python has no equivalent (use None)"},
    {"\\bconst\\s+\\w+\\s*=", "const is JavaScript — in Python, just assign variables"},
    {"\\blet\\s+\\w+\\s*=", "let is JavaScript — in Python, just assign variables"},
    {"\\bvar\\s+\\w+\\s*=", "var is JavaScript — in Python, just assign variables"},
    // Made-up Python functions
    {"json\\.stringify\\(", "json.stringify() is JavaScript — in Python, use json.dumps()"},
    {"json\\.parse\\(", "json.parse() is JavaScript — in Python, use json.loads()"},
    {"Math\\.round\\(", "Math.round() is JavaScript — in Python, use round()"},
    {"Math\\.floor\\(", "Math.floor() is JavaScript — in Python, use math.floor() or int()"},
    {"Math\\.ceil\\(", "Math.ceil() is JavaScript — in Python, use math.ceil()"},
    {"Math\\.abs\\(", "Math.abs() is JavaScript — in Python, use abs()"},
    {"Math\\.random\\(", "Math.random() is JavaScript — in Python, use random.random()"},
    {"Math\\.max\\(", "Math.max() is JavaScript — in Python, use max()"},
    {"Math\\.min\\(", "Math.min() is JavaScript — in Python, use min()"},
    {"list\\.flatten\\(", "list.flatten() doesn't exist — use itertools.chain.from_iterable()"},
    {"dict\\.to_json\\(", "dict.to_json() doesn't exist — use json.dumps()"},
    {"\\.toInt\\(", ".toInt() doesn't exist in Python — use int()"},
    {"\\.toFloat\\(", ".toFloat() doesn't exist in Python — use float()"},
    {"\\.size\\(\\)", ".size() doesn't exist for sequences in Python — use len()"},
    {"\\bArray\\(", "Array() is JavaScript — in Python, use list()"},
    {"Object\\.keys\\(", "Object.keys() is JavaScript — in Python, use .keys()"},
    {"Object\\.values\\(", "Object.values() is JavaScript — in Python, use .values()"},
    {"\\bString\\(", "String() is JavaScript — in Python, use str()"},
    {"\\bNumber\\(", "Number() is JavaScript — in Python, use int() or float()"},
    {"\\bBoolean\\(", "Boolean() is JavaScript — in Python, use bool()"},
    // Python self in wrong context
    {"\\bthis\\.\\w+", "this.x is JavaScript — in Python, use self.x"},
    {"\\basync\\s+function\\b", "async function is JavaScript — in Python, use async def"},
};

// JavaScript patterns: Python syntax/builtins used in JS
static const std::vector<std::pair<std::string, std::string>> JS_HALLUCINATION_PATTERNS = {
    // Python builtins in JS
    {"\\bprint\\(", "print() is Python — in JavaScript, use console.log()"},
    {"\\blen\\(", "len() is Python — in JavaScript, use .length"},
    {"\\brange\\(", "range() is Python — in JavaScript, use for loop or Array.from()"},
    {"\\bdef\\s+\\w+", "def is Python — in JavaScript, use function or arrow functions"},
    {"\\belif\\b", "elif is Python — in JavaScript, use else if"},
    {"\\bTrue\\b", "True is Python — in JavaScript, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in JavaScript, use false (lowercase)"},
    {"\\bNone\\b", "None is Python — in JavaScript, use null"},
    {"\\band\\b(?=\\s)", "and is Python — in JavaScript, use &&"},
    {"\\bor\\b(?=\\s)", "or is Python — in JavaScript, use ||"},
    {"\\bnot\\b(?=\\s)", "not is Python — in JavaScript, use !"},
    {"\\b__\\w+__\\b", "Dunder methods (__x__) are Python — no equivalent in JavaScript"},
    // Python methods in JS
    {"\\.append\\(", ".append() is Python — in JavaScript, use .push()"},
    {"\\.extend\\(", ".extend() is Python — in JavaScript, use .concat() or spread"},
    {"\\.strip\\(", ".strip() is Python — in JavaScript, use .trim()"},
    {"\\.upper\\(", ".upper() is Python — in JavaScript, use .toUpperCase()"},
    {"\\.lower\\(", ".lower() is Python — in JavaScript, use .toLowerCase()"},
    {"\\.items\\(\\)", ".items() is Python — in JavaScript, use Object.entries()"},
    // Made-up JS functions
    {"\\barray\\.contains\\(", "array.contains() doesn't exist — use .includes()"},
    {"\\bstring\\.contains\\(", "string.contains() doesn't exist — use .includes()"},
    {"Array\\.flatten\\(", "Array.flatten() doesn't exist — use .flat()"},
    {"JSON\\.load\\(", "JSON.load() is Python-style — in JavaScript, use JSON.parse()"},
    {"JSON\\.dump\\(", "JSON.dump() is Python-style — in JavaScript, use JSON.stringify()"},
    {"console\\.write\\(", "console.write() doesn't exist — use console.log()"},
    {"Math\\.sum\\(", "Math.sum() doesn't exist — use array.reduce((a,b) => a+b, 0)"},
    {"fs\\.readfile\\(", "fs.readfile() wrong case — use fs.readFile() or fs.readFileSync()"},
    // Python self/class in JS
    {"\\bself\\.\\w+", "self.x is Python — in JavaScript, use this.x"},
    {"\\basync\\s+def\\b", "async def is Python — in JavaScript, use async function"},
};

// Go patterns: Python/JS idioms in Go
static const std::vector<std::pair<std::string, std::string>> GO_HALLUCINATION_PATTERNS = {
    {"\\bprint\\(", "print() is Python — in Go, use fmt.Print() or fmt.Println()"},
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Go, use fmt.Println()"},
    {"\\bNone\\b", "None is Python — in Go, use nil"},
    {"\\bTrue\\b", "True is Python — in Go, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Go, use false (lowercase)"},
    {"\\bnull\\b", "null is JavaScript — in Go, use nil"},
    {"\\blen\\(", "len() is a builtin in Go too, but ensure you import fmt for printing"},
    {"\\.append\\(", ".append() is Python — in Go, use append(slice, elem)"},
    {"\\.push\\(", ".push() is JavaScript — in Go, use append(slice, elem)"},
    {"\\bdef\\s+\\w+", "def is Python — in Go, use func"},
    {"\\bfunction\\s+\\w+", "function is JavaScript — in Go, use func"},
    // FIX-DX-11: Additional cross-language patterns
    {"\\.forEach\\(", ".forEach() is JavaScript — in Go, use for _, v := range slice"},
    {"\\.map\\(", ".map() is JavaScript — in Go, use a for loop with append"},
    {"\\.filter\\(", ".filter() is JavaScript — in Go, use a for loop with condition"},
};

// Ruby patterns: Python/JS idioms in Ruby
static const std::vector<std::pair<std::string, std::string>> RUBY_HALLUCINATION_PATTERNS = {
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Ruby, use puts or print"},
    {"\\bnull\\b", "null is JavaScript — in Ruby, use nil"},
    {"\\bNone\\b", "None is Python — in Ruby, use nil"},
    {"\\bTrue\\b", "True is Python — in Ruby, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Ruby, use false (lowercase)"},
    {"\\bdef\\s+\\w+\\s*\\(", "Python-style def — in Ruby, use def without parens in definition"},
    {"\\blen\\(", "len() is Python — in Ruby, use .length or .size"},
    {"\\bprint\\(", "print() with parens — in Ruby, use puts (adds newline) or print without parens"},
};

// Shell patterns: Python/JS idioms in Shell
static const std::vector<std::pair<std::string, std::string>> SHELL_HALLUCINATION_PATTERNS = {
    {"\\bprint\\(", "print() is Python — in Shell, use echo"},
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Shell, use echo"},
    {"(?:^|[=\\s(,])null(?:\\s|$|[);,])", "null is JavaScript — in Shell, variables are unset or empty strings"},
    {"\\bNone\\b", "None is Python — in Shell, use empty string or unset"},
    {"\\bTrue\\b", "True is Python — in Shell, use true (lowercase command)"},
    {"\\bFalse\\b", "False is Python — in Shell, use false (lowercase command)"},
    {"\\bdef\\s+\\w+", "def is Python — in Shell, use function_name() { ... }"},
    {"\\bimport\\s+", "import is Python — in Shell, use source or . to include files"},
};

// Nim patterns: Python/JS idioms in Nim (FIX-DX-11: expanded)
static const std::vector<std::pair<std::string, std::string>> NIM_HALLUCINATION_PATTERNS = {
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Nim, use echo"},
    {"\\bnull\\b", "null is JavaScript — in Nim, use nil"},
    {"\\bNone\\b", "None is Python — in Nim, use nil"},
    {"\\bTrue\\b", "True is Python — in Nim, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Nim, use false (lowercase)"},
    {"\\bdef\\s+\\w+", "def is Python — in Nim, use proc or func"},
    {"\\blen\\(", "len() is also valid in Nim — make sure to use it without import"},
    {"\\.upper\\(", ".upper() is Python — in Nim, use .toUpperAscii()"},
    {"\\.lower\\(", ".lower() is Python — in Nim, use .toLowerAscii()"},
    {"\\.append\\(", ".append() is Python — in Nim, use .add()"},
    {"\\.strip\\(\\)", ".strip() is Python — in Nim, use strip() from strutils (import strutils)"},
    {"\\bprint\\(", "print() is Python — in Nim, use echo"},
};

// Cross-language confusion patterns
static const std::vector<std::pair<std::string, std::string>> CROSS_LANG_PATTERNS = {
    {"#\\s+\\w", "# comments are Python/Ruby — in JavaScript, use //"},
    {"//\\s+\\w", "// comments are JS/C++ — in Python, use #"},
};

// Strip comments from code based on language syntax.
// Replaces comment content with spaces (preserving line structure for regex).
// Must be called AFTER string stripping to avoid matching # or // inside strings.
static std::string stripComments(const std::string& code, const std::string& language) {
    bool uses_hash = (language == "python" || language == "ruby" || language == "rb" ||
                      language == "shell" || language == "bash" || language == "sh" ||
                      language == "nim");
    bool uses_slashslash = (language == "javascript" || language == "js" || language == "node" ||
                            language == "go" || language == "golang" ||
                            language == "cpp" || language == "c++" ||
                            language == "rust" || language == "csharp" || language == "cs");
    bool uses_block = (language == "javascript" || language == "js" || language == "node" ||
                       language == "go" || language == "golang" ||
                       language == "cpp" || language == "c++" ||
                       language == "rust" || language == "csharp" || language == "cs");
    // V-GOV-002: add -- line comment style for SQL, Lua, and similar languages.
    // Without this, `-- DROP TABLE users` in a <<sql block is not stripped and the
    // governance scanner sees "DROP TABLE users" as active code (false positive or bypass).
    bool uses_dash_dash = (language == "sql" || language == "lua" ||
                           language == "haskell" || language == "ada");

    std::string result;
    result.reserve(code.size());

    for (size_t i = 0; i < code.size(); ++i) {
        // Check for block comments /* ... */
        if (uses_block && i + 1 < code.size() && code[i] == '/' && code[i + 1] == '*') {
            // Replace with spaces until closing */
            result += ' ';
            result += ' ';
            i += 2;
            while (i < code.size()) {
                if (i + 1 < code.size() && code[i] == '*' && code[i + 1] == '/') {
                    result += ' ';
                    result += ' ';
                    i += 1; // outer loop does +1
                    break;
                }
                result += (code[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            continue;
        }

        // Check for // line comments
        if (uses_slashslash && i + 1 < code.size() && code[i] == '/' && code[i + 1] == '/') {
            // Replace rest of line with spaces
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n'; // preserve the newline
            continue;
        }

        // Check for # line comments
        if (uses_hash && code[i] == '#') {
            // Shell special case: #! (shebang) at very start is still a comment — strip it
            // Python: # at line start or after whitespace/code is always a comment
            // (strings already stripped, so no risk of matching # inside strings)
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n';
            continue;
        }

        // V-GOV-002: -- line comment (SQL, Lua, Haskell, Ada)
        if (uses_dash_dash && i + 1 < code.size() && code[i] == '-' && code[i+1] == '-') {
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n';
            continue;
        }

        result += code[i];
    }
    return result;
}

std::string GovernanceEngine::checkHallucinatedApis(const std::string& language,
                                                     const std::string& code, int line) {
    auto& cfg = rules().code_quality.no_hallucinated_apis;
    if (!cfg.enabled) return "";
    clearTrace();

    // Select patterns based on language
    const std::vector<std::pair<std::string, std::string>>* lang_patterns = nullptr;
    if (language == "python") lang_patterns = &PYTHON_HALLUCINATION_PATTERNS;
    else if (language == "javascript" || language == "js" || language == "node")
        lang_patterns = &JS_HALLUCINATION_PATTERNS;
    else if (language == "go" || language == "golang")
        lang_patterns = &GO_HALLUCINATION_PATTERNS;
    else if (language == "ruby" || language == "rb")
        lang_patterns = &RUBY_HALLUCINATION_PATTERNS;
    else if (language == "shell" || language == "bash" || language == "sh")
        lang_patterns = &SHELL_HALLUCINATION_PATTERNS;
    else if (language == "nim")
        lang_patterns = &NIM_HALLUCINATION_PATTERNS;

    // Strip string literal contents before checking patterns.
    // This prevents false positives when code generates source code for
    // another language inside strings (e.g., Go code that builds Rust source).
    std::string code_no_strings;
    {
        bool in_single = false, in_double = false, in_backtick = false;
        bool escaped = false;
        for (size_t i = 0; i < code.size(); ++i) {
            char c = code[i];
            if (escaped) { escaped = false; continue; }
            // C11 fix: track escapes in backtick strings too
            if (c == '\\' && (in_single || in_double || in_backtick)) { escaped = true; continue; }
            if (c == '"' && !in_single && !in_backtick) { in_double = !in_double; continue; }
            if (c == '\'' && !in_double && !in_backtick) { in_single = !in_single; continue; }
            if (c == '`' && !in_double && !in_single) { in_backtick = !in_backtick; continue; }
            if (!in_single && !in_double && !in_backtick) {
                code_no_strings += c;
            }
        }
    }

    // Strip comments for the target language (after string stripping).
    // This prevents false positives like "# TODO: use append" in Shell
    // triggering Python hallucination patterns, or "// use len()" in Python
    // triggering JS hallucination patterns.
    std::string code_clean = stripComments(code_no_strings, language);

    // Check language-specific patterns (on code with strings AND comments stripped)
    if (lang_patterns) {
        for (const auto& [pattern, suggestion] : *lang_patterns) {
            try {
                auto flags = cfg.case_sensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase);
                std::regex re(pattern, flags);
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
                    // C1: Prepend language context + C2: Add NAAb equivalent hint
                    std::string full_suggestion =
                        fmt::format("Inside <<{}>> polyglot block:\n  {}\n\n"
                            "Note: In NAAb native code (outside << >> blocks), use NAAb stdlib.\n"
                            "  Arrays: array.push(arr, item), array.pop(arr)\n"
                            "  Strings: string.upper(s), string.lower(s), string.split(s, \",\")\n"
                            "  Length: len(x)", language, suggestion);
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language syntax in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            full_suggestion, "", ""));
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }
    }

    // Check cross-language confusion patterns (on string-stripped code,
    // but NOT comment-stripped — we WANT to find wrong-language comments)
    if (cfg.check_cross_language) {
        // Only check relevant cross-language patterns
        if (language == "python") {
            // Check for JS comment style in Python (// not stripped by Python comment rules)
            try {
                std::regex re("(^|\\n)\\s*//\\s+");
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language confusion in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            "// comments are JavaScript — in Python, use #",
                            "// this is wrong in Python",
                            "# this is correct in Python"));
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        } else if (language == "javascript" || language == "js") {
            // Check for Python comment style in JS (# not stripped by JS comment rules)
            try {
                std::regex re("(^|\\n)\\s*#\\s+");
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language confusion in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            "# comments are Python — in JavaScript, use //",
                            "# this is wrong in JavaScript",
                            "// this is correct in JavaScript"));
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }
    }

    // Check custom patterns (on cleaned code)
    if (!cfg.custom_patterns.empty()) {
        std::string found = searchPatterns(code_clean, cfg.custom_patterns);
        if (!found.empty()) {
            return enforce("code_quality.no_hallucinated_apis", cfg.level,
                formatError(cfg.level,
                    fmt::format("Hallucinated API pattern in {} block: \"{}\"", language, found),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "code_quality.no_hallucinated_apis",
                    "This API call doesn't exist in the target language.\n"
                    "Check language documentation for correct function names and syntax.", "", ""));
        }
    }

    recordPass("code_quality.no_hallucinated_apis", cfg.level);
    return "";
}

// ==========================================================================
// Semantic Checks — targeted validation for Python, JavaScript, Shell
// ==========================================================================

// Known Python stdlib modules + common third-party
static const std::unordered_set<std::string> PYTHON_KNOWN_MODULES = {
    // Python 3.x stdlib
    "abc", "aifc", "argparse", "array", "ast", "asynchat", "asyncio", "asyncore",
    "atexit", "audioop", "base64", "bdb", "binascii", "binhex", "bisect",
    "builtins", "bz2", "calendar", "cgi", "cgitb", "chunk", "cmath", "cmd",
    "code", "codecs", "codeop", "collections", "colorsys", "compileall",
    "concurrent", "configparser", "contextlib", "contextvars", "copy", "copyreg",
    "cProfile", "crypt", "csv", "ctypes", "curses", "dataclasses", "datetime",
    "dbm", "decimal", "difflib", "dis", "distutils", "doctest", "email",
    "encodings", "enum", "errno", "faulthandler", "fcntl", "filecmp", "fileinput",
    "fnmatch", "formatter", "fractions", "ftplib", "functools", "gc", "getopt",
    "getpass", "gettext", "glob", "grp", "gzip", "hashlib", "heapq", "hmac",
    "html", "http", "idlelib", "imaplib", "imghdr", "imp", "importlib",
    "inspect", "io", "ipaddress", "itertools", "json", "keyword", "lib2to3",
    "linecache", "locale", "logging", "lzma", "mailbox", "mailcap", "marshal",
    "math", "mimetypes", "mmap", "modulefinder", "multiprocessing", "netrc",
    "nis", "nntplib", "numbers", "operator", "optparse", "os", "ossaudiodev",
    "parser", "pathlib", "pdb", "pickle", "pickletools", "pipes", "pkgutil",
    "platform", "plistlib", "poplib", "posix", "posixpath", "pprint",
    "profile", "pstats", "pty", "pwd", "py_compile", "pyclbr", "pydoc",
    "queue", "quopri", "random", "re", "readline", "reprlib", "resource",
    "rlcompleter", "runpy", "sched", "secrets", "select", "selectors",
    "shelve", "shlex", "shutil", "signal", "site", "smtpd", "smtplib",
    "sndhdr", "socket", "socketserver", "sqlite3", "ssl", "stat", "statistics",
    "string", "stringprep", "struct", "subprocess", "sunau", "symtable",
    "sys", "sysconfig", "syslog", "tabnanny", "tarfile", "telnetlib", "tempfile",
    "termios", "test", "textwrap", "threading", "time", "timeit", "tkinter",
    "token", "tokenize", "tomllib", "trace", "traceback", "tracemalloc",
    "tty", "turtle", "turtledemo", "types", "typing", "unicodedata",
    "unittest", "urllib", "uu", "uuid", "venv", "warnings", "wave",
    "weakref", "webbrowser", "winreg", "winsound", "wsgiref", "xdrlib",
    "xml", "xmlrpc", "zipapp", "zipfile", "zipimport", "zlib",
    // Internal/private modules that are valid
    "_thread", "_io", "_collections", "_functools", "_operator",
    // Common third-party
    "numpy", "np", "pandas", "pd", "requests", "flask", "django",
    "scipy", "matplotlib", "plt", "sklearn", "tensorflow", "tf",
    "torch", "cv2", "PIL", "pillow", "sqlalchemy", "celery",
    "pytest", "setuptools", "pip", "yaml", "pyyaml", "toml",
    "aiohttp", "httpx", "fastapi", "uvicorn", "pydantic",
    "beautifulsoup4", "bs4", "lxml", "scrapy", "selenium",
    "boto3", "botocore", "paramiko", "fabric", "click",
    "rich", "tqdm", "colorama", "pygments", "jinja2",
    "cryptography", "bcrypt", "jwt", "dotenv", "six", "attrs",
    "dateutil", "pytz", "tzdata", "chardet", "certifi",
    "urllib3", "idna", "packaging", "appdirs", "distlib",
    "virtualenv", "wheel", "twine", "black", "flake8",
    "mypy", "pylint", "isort", "autopep8", "bandit",
    "gunicorn", "gevent", "twisted", "tornado",
    "redis", "pymongo", "psycopg2", "mysql",
    "networkx", "sympy", "nltk", "spacy",
};

// Known Node.js builtins + common third-party
static const std::unordered_set<std::string> JS_KNOWN_MODULES = {
    // Node.js builtins
    "assert", "buffer", "child_process", "cluster", "console", "constants",
    "crypto", "dgram", "dns", "domain", "events", "fs", "http", "http2",
    "https", "inspector", "module", "net", "os", "path", "perf_hooks",
    "process", "punycode", "querystring", "readline", "repl", "stream",
    "string_decoder", "sys", "timers", "tls", "trace_events", "tty",
    "url", "util", "v8", "vm", "wasi", "worker_threads", "zlib",
    // Common third-party
    "express", "lodash", "underscore", "axios", "moment", "dayjs",
    "react", "vue", "angular", "svelte", "next", "nuxt",
    "webpack", "babel", "typescript", "esbuild", "vite", "rollup",
    "jest", "mocha", "chai", "sinon", "supertest", "vitest",
    "mongoose", "sequelize", "prisma", "knex", "typeorm",
    "dotenv", "cors", "helmet", "morgan", "winston", "pino",
    "ws", "redis", "ioredis", "pg", "mysql2",
    "uuid", "chalk", "commander", "yargs", "inquirer",
    "cheerio", "puppeteer", "sharp", "multer",
    "jsonwebtoken", "bcrypt", "bcryptjs", "passport",
};

// Known shell commands
static const std::unordered_set<std::string> SHELL_KNOWN_COMMANDS = {
    // Core POSIX/GNU utilities
    "echo", "printf", "cat", "head", "tail", "wc", "sort", "uniq",
    "grep", "egrep", "fgrep", "sed", "awk", "cut", "tr", "paste",
    "ls", "cp", "mv", "rm", "mkdir", "rmdir", "touch", "stat",
    "chmod", "chown", "chgrp", "ln", "find", "xargs", "tee",
    "cd", "pwd", "pushd", "popd", "export", "source", "eval",
    "read", "test", "expr", "true", "false", "yes", "sleep",
    "date", "cal", "who", "whoami", "id", "groups", "hostname",
    "uname", "uptime", "df", "du", "free", "top", "ps", "kill",
    "bg", "fg", "jobs", "wait", "nohup", "nice", "time", "timeout",
    "tar", "gzip", "gunzip", "bzip2", "zip", "unzip", "xz",
    "curl", "wget", "ssh", "scp", "rsync", "nc", "ping", "dig", "nslookup",
    "git", "docker", "make", "cmake", "python", "python3", "node", "npm", "npx",
    "pip", "pip3", "ruby", "perl", "java", "javac", "go", "rustc", "cargo",
    "jq", "yq", "xmllint", "base64", "md5sum", "sha256sum", "shasum",
    "diff", "patch", "comm", "join", "bc", "dc",
    "env", "set", "unset", "trap", "shift", "getopts", "local", "return",
    "if", "then", "else", "elif", "fi", "for", "do", "done", "while",
    "until", "case", "esac", "select", "in", "function",
    "break", "continue", "exit",
    // Common tools
    "apt", "apt-get", "brew", "yum", "dnf", "pacman", "pkg",
    "systemctl", "service", "journalctl",
    "crontab", "at", "screen", "tmux",
    "vim", "vi", "nano", "less", "more", "man",
    "which", "whereis", "type", "file", "realpath", "dirname", "basename",
};

// Shell command typos → suggestions
static const std::unordered_map<std::string, std::string> SHELL_COMMAND_TYPOS = {
    {"grpe", "grep"}, {"grrep", "grep"}, {"greep", "grep"},
    {"awke", "awk"}, {"sde", "sed"}, {"mkdri", "mkdir"},
    {"chmd", "chmod"}, {"cta", "cat"}, {"ehco", "echo"},
    {"pritnf", "printf"}, {"pyhton", "python"}, {"pytohn", "python"},
    {"touhc", "touch"}, {"rn", "rm"}, {"mdkir", "mkdir"},
    {"whcih", "which"}, {"wegt", "wget"}, {"curlr", "curl"},
    {"ecoh", "echo"}, {"caht", "cat"}, {"grp", "grep"},
};

// API signature check structure
struct SemanticCheck {
    std::string pattern;
    std::string message;
    std::string suggestion;
};

// Python API misuse patterns
static const std::vector<SemanticCheck> PYTHON_API_CHECKS = {
    {"range\\([^)]*\\d+\\.\\d+", "range() only accepts integers, not floats", "Use int(): range(int(n)) or use numpy.arange() for float ranges"},
    {"re\\.sub\\(\\s*[^,)]+\\s*\\)", "re.sub() needs 3 args: pattern, replacement, string", "re.sub(r'pattern', 'replacement', text)"},
    {"sorted\\([^)]*,\\s*reverse\\s*[^=\\s)]", "sorted() reverse parameter needs =True or =False", "sorted(lst, reverse=True)"},
    {"=\\s*\\w+\\.sort\\(\\s*\\)", ".sort() returns None — it sorts in-place, don't assign result", "lst.sort()  # modifies lst in-place; or use sorted(lst)"},
    {"=\\s*\\w+\\.update\\(", ".update() returns None — it modifies dict in-place, don't assign result", "d.update(other)  # modifies d; or use {**d, **other}"},
    {"\\.split\\(\\s*\\d+\\s*\\)", "str.split() takes a string delimiter, not an integer", "s.split(',') or s.split() for whitespace"},
    {"int\\(\\s*\\d+\\s*,\\s*\\d+", "int() with base requires string first arg, not int literal", "int('ff', 16) or int('101', 2)"},
};

// Python API checks that need RAW code (patterns involve string literals)
static const std::vector<SemanticCheck> PYTHON_API_RAW_CHECKS = {
    {"json\\.dumps\\(\\s*\\)", "json.dumps() requires at least 1 argument", "json.dumps(data)"},
    {"json\\.loads\\(\\s*\\)", "json.loads() requires at least 1 argument", "json.loads(string)"},
    {"open\\([^)]*['\"](?:rw|wr)['\"]", "Invalid file mode — 'rw'/'wr' don't exist", "Use 'r+' for read/write or 'w+' for write/read: open(f, 'r+')"},
};

// Python dangerous eval/exec patterns — RAW code (f-string/quote patterns)
static const std::vector<SemanticCheck> PYTHON_EVAL_RAW_CHECKS = {
    {"eval\\(\\s*f['\"]", "eval() with f-string is dangerous — user input can execute arbitrary code", "Use ast.literal_eval() for safe parsing of literals"},
    {"exec\\(\\s*f['\"]", "exec() with f-string is dangerous — arbitrary code execution", "Avoid exec() with dynamic strings; use safer alternatives"},
};

// Python dangerous eval/exec patterns — CLEANED code (string content stripped)
static const std::vector<SemanticCheck> PYTHON_EVAL_CHECKS = {
    {"eval\\([^)]*\\+\\s*\\w", "eval() with string concatenation is dangerous — injection risk", "Use ast.literal_eval() or json.loads() for safe parsing"},
    {"__import__\\(", "__import__() dynamic import is hard to audit and potentially dangerous", "Use regular import statements for clarity and safety"},
    // Obfuscation hardening: compile()+eval, globals chain, __builtins__, __loader__, types.ModuleType, chr chain
    {"\\bcompile\\s*\\(.*['\"]eval['\"]", "compile() with eval mode is dangerous — dynamic code execution", "Use direct function calls instead of compile()"},
    {"globals\\s*\\(\\s*\\)\\s*\\[", "globals() dict access — potential dynamic import chain", "Use standard import statements"},
    {"__builtins__\\s*(?:\\.|\\[)", "Direct __builtins__ access — can bypass import restrictions", "Use standard import statements"},
    {"__loader__\\s*\\.", "__loader__ access — can load arbitrary modules", "Use standard import statements"},
    {"types\\.ModuleType\\s*\\(", "types.ModuleType() — dynamic module creation", "Use standard import statements"},
    {"chr\\s*\\(\\s*\\d+\\s*\\)\\s*\\+\\s*chr\\s*\\(\\s*\\d+\\s*\\)\\s*\\+\\s*chr", "chr() chain (3+) — likely string obfuscation to hide imports", "Use literal strings instead of character code chains"},
    {"__subclasses__\\s*\\(", "__subclasses__() — class hierarchy introspection can escape sandboxes", "Avoid __subclasses__() in governed code"},
    {"\\b__mro__\\b", "__mro__ — method resolution order access can walk to dangerous base classes", "Avoid __mro__ in governed code"},
};

// JavaScript API misuse patterns
static const std::vector<SemanticCheck> JS_API_CHECKS = {
    {"Array\\.isArray\\(\\s*\\)", "Array.isArray() requires 1 argument", "Array.isArray(value)"},
    {"new\\s+Promise\\(\\s*\\)", "new Promise() requires an executor function", "new Promise((resolve, reject) => { ... })"},
};

// JS API checks needing RAW code (patterns involve string/backtick delimiters)
static const std::vector<SemanticCheck> JS_API_RAW_CHECKS = {
    {"JSON\\.parse\\(\\s*\\)", "JSON.parse() requires at least 1 argument", "JSON.parse(jsonString)"},
    {"JSON\\.stringify\\(\\s*\\)", "JSON.stringify() requires at least 1 argument", "JSON.stringify(data)"},
    {"set(?:Timeout|Interval)\\(\\s*['\"]", "setTimeout/setInterval with string arg is like eval() — security risk", "Use a function: setTimeout(() => { ... }, ms)"},
};

// JavaScript dangerous patterns (need RAW code — patterns include backtick/quote chars)
static const std::vector<SemanticCheck> JS_EVAL_CHECKS = {
    {"eval\\(\\s*`", "eval() with template literal is dangerous — arbitrary code execution", "Use JSON.parse() for data or a safe parser"},
    {"\\.innerHTML\\s*=\\s*[^'\"`\\s;]", "Setting innerHTML with a variable risks XSS attacks", "Use .textContent for plain text, or sanitize with DOMPurify"},
    {"document\\.write\\(", "document.write() is dangerous and can overwrite the entire page", "Use DOM methods: .appendChild(), .textContent, .innerHTML with sanitized input"},
    // Obfuscation hardening: dynamic import()
    {"import\\s*\\(\\s*['\"]", "Dynamic import() can load arbitrary modules at runtime", "Use static import/require statements"},
};

// Shell syntax validation patterns
static const std::vector<SemanticCheck> SHELL_SYNTAX_CHECKS = {
    {"\\[(?!\\[)[^ \\t\\n]", "Missing space after '[' in test command", "Use spaces: [ -f file ] not [-f file]"},
    {"[^ \\t\\n]\\](?!\\])", "Missing space before ']' in test command", "Use spaces: [ -f file ] not [ -f file]"},
    {"\\[\\s+[^\\]]*\\s+==\\s+", "'==' in [ ] is not POSIX — may fail on some shells", "Use '=' in [ ] or use [[ ]] for '==': [ \"$x\" = \"$y\" ]"},
    {"\\]\\s+then\\b", "Missing ';' or newline between ']' and 'then'", "if [ condition ]; then  # add semicolon"},
};

// Shell safety patterns
static const std::vector<SemanticCheck> SHELL_SAFETY_CHECKS = {
    {"rm\\s+-[rR]f\\s+/\\s", "rm -rf / is extremely dangerous — will destroy the filesystem", "Never use rm -rf / — specify exact paths"},
    {"rm\\s+-[rR]f\\s+/\\n", "rm -rf / is extremely dangerous — will destroy the filesystem", "Never use rm -rf / — specify exact paths"},
    {"rm\\s+-[rR]f\\s+\"?\\$\\{?\\w*\\}?\"?/", "rm -rf with variable — if variable is empty, becomes rm -rf /", "Guard: [ -n \"$VAR\" ] && rm -rf \"$VAR/path\""},
    // Obfuscation hardening: hex/octal escape sequences in $'...'
    {"\\$['\"]\\\\x[0-9a-fA-F]", "Hex escape in $'...' — potential command obfuscation", "Use literal command names — hex escapes can hide dangerous commands"},
    {"\\$['\"]\\\\[0-7]{3}", "Octal escape in $'...' — potential command obfuscation", "Use literal command names — octal escapes can hide dangerous commands"},
};

// Unquoted variable check for shell (separate because more complex)
static const std::vector<SemanticCheck> SHELL_UNQUOTED_VAR_CHECKS = {
    {"(?:rm|mv|cp)\\s+(?:-[a-zA-Z]+\\s+)*[^\"']*\\$[a-zA-Z_]", "Unquoted variable in rm/mv/cp — empty variable causes unexpected behavior", "Always quote variables: rm \"$file\" not rm $file"},
};

// --- Language-specific safety checks for languages missing semantic coverage ---

static const std::vector<SemanticCheck> GO_SAFETY_CHECKS = {
    {"\\bunsafe\\.Pointer\\b", "unsafe.Pointer bypasses Go's type system — memory safety risk", "Avoid unsafe package in governed code"},
    {"exec\\.Command\\s*\\(", "os/exec.Command() — command execution", "Use NAAb stdlib process.run()"},
};

static const std::vector<SemanticCheck> RUST_SAFETY_CHECKS = {
    {"\\bunsafe\\s*\\{", "unsafe block bypasses Rust's safety guarantees", "Avoid unsafe in governed polyglot code"},
    {"Command::new\\s*\\(", "std::process::Command — command execution", "Use NAAb stdlib process.run()"},
    {"\\blibc::", "Raw libc FFI calls bypass safety", "Avoid libc in governed polyglot code"},
};

static const std::vector<SemanticCheck> CPP_SAFETY_CHECKS = {
    {"\\bsystem\\s*\\(", "system() — command execution", "Use NAAb stdlib process.run()"},
    {"\\bpopen\\s*\\(", "popen() — command execution with pipe", "Use NAAb stdlib process.run()"},
    {"\\b__asm__\\b|\\basm\\s*\\(", "Inline assembly bypasses all safety", "Avoid inline assembly in governed code"},
    {"\\bdlopen\\s*\\(", "dlopen() — dynamic library loading", "Avoid dynamic library loading in governed code"},
};

static const std::vector<SemanticCheck> CSHARP_SAFETY_CHECKS = {
    {"Process\\.Start\\s*\\(", "Process.Start() — command execution", "Use NAAb stdlib process.run()"},
    {"\\bunsafe\\s*\\{", "unsafe block — raw pointer operations", "Avoid unsafe code in governed blocks"},
    {"\\bDllImport\\b", "DllImport — native interop", "Avoid P/Invoke in governed blocks"},
};

static const std::vector<SemanticCheck> RUBY_SAFETY_CHECKS = {
    {"\\bsystem\\s*\\(", "system() — command execution", "Use NAAb stdlib process.run()"},
    {"\\beval\\s*\\(", "eval() — arbitrary code execution", "Avoid dynamic code execution"},
    {"\\binstance_eval\\b", "instance_eval — arbitrary code in object context", "Use direct method calls"},
    {"\\bsend\\s*\\(", "send() — dynamic method dispatch", "Use direct method calls"},
};

static const std::vector<SemanticCheck> PHP_SAFETY_CHECKS = {
    {"\\bsystem\\s*\\(", "system() — command execution", "Use NAAb stdlib process.run()"},
    {"\\bexec\\s*\\(", "exec() — command execution", "Use NAAb stdlib process.run()"},
    {"\\beval\\s*\\(", "eval() — arbitrary code execution", "Avoid dynamic code execution"},
    {"\\bshell_exec\\s*\\(", "shell_exec() — command execution", "Use NAAb stdlib process.run()"},
    {"\\bpassthru\\s*\\(", "passthru() — command execution with output", "Use NAAb stdlib process.run()"},
};

static const std::vector<SemanticCheck> NIM_SAFETY_CHECKS = {
    {"\\bexecProcess\\s*\\(", "execProcess() — command execution", "Use NAAb stdlib process.run()"},
    {"\\bstartProcess\\s*\\(", "startProcess() — command execution", "Use NAAb stdlib process.run()"},
    {"\\{\\.importc\\b", "importc pragma — C FFI", "Avoid FFI in governed code"},
    {"\\{\\.emit\\b", "emit pragma — raw code injection", "Avoid emit in governed code"},
};

std::string GovernanceEngine::checkSemanticIssues(
    const std::string& language, const std::string& code, int line) {

    auto& cfg = rules().code_quality.semantic_checks;
    if (!cfg.enabled) return "";
    clearTrace();

    // Strip strings and comments for pattern matching
    std::string code_no_strings = stripStringLiterals(code);
    std::string clean = stripComments(code_no_strings, language);

    // Helper: run a vector of SemanticCheck patterns against a target string
    auto runChecksOn = [&](const std::vector<SemanticCheck>& checks,
                           const std::string& target) -> std::string {
        for (const auto& check : checks) {
            try {
                std::regex re(check.pattern, std::regex::ECMAScript);
                std::smatch match;
                if (std::regex_search(target, match, re)) {
                    return enforce("code_quality.semantic_checks", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Semantic issue in {} block: {}", language, check.message),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.semantic_checks",
                            check.suggestion, "", ""));
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }
        return "";
    };
    // Convenience: run against cleaned (string+comment stripped) code
    auto runChecks = [&](const std::vector<SemanticCheck>& checks) -> std::string {
        return runChecksOn(checks, clean);
    };
    // Run against raw code (for patterns involving string literals like open("rw"), eval(f"..."))
    auto runChecksRaw = [&](const std::vector<SemanticCheck>& checks) -> std::string {
        return runChecksOn(checks, code);
    };

    // === PYTHON ===
    if (language == "python") {
        // 1. Import validation
        if (cfg.check_imports) {
            try {
                std::regex import_re("(?:^|\\n)\\s*(?:import\\s+(\\w+)|from\\s+(\\w+)\\s+import)",
                                     std::regex::ECMAScript);
                auto begin = std::sregex_iterator(clean.begin(), clean.end(), import_re);
                auto end_it = std::sregex_iterator();
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
                    if (PYTHON_KNOWN_MODULES.find(mod) == PYTHON_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown Python module '{}' — verify it's installed", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "This module is not in Python's stdlib or common packages.\n"
                                "  If it's a project dependency, ensure it's in requirements.txt.",
                                fmt::format("import {}", mod),
                                "import json  # use stdlib modules"));
                    }
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }

        // 2. API signature checks (cleaned code for most, raw for string-literal patterns)
        if (cfg.check_api_signatures) {
            std::string err = runChecks(PYTHON_API_CHECKS);
            if (!err.empty()) return err;
            // open() mode check needs raw code (mode is a string literal)
            err = runChecksRaw(PYTHON_API_RAW_CHECKS);
            if (!err.empty()) return err;
        }

        // 3. Dangerous eval/exec
        if (cfg.check_dangerous_eval) {
            // f-string patterns need raw code (quote chars stripped otherwise)
            std::string err = runChecksRaw(PYTHON_EVAL_RAW_CHECKS);
            if (!err.empty()) return err;
            // Concatenation/__import__ patterns use cleaned code
            err = runChecks(PYTHON_EVAL_CHECKS);
            if (!err.empty()) return err;
        }
    }

    // === JAVASCRIPT ===
    else if (language == "javascript" || language == "js" || language == "node") {
        // 1. Import/require validation (RAW code — string literals contain module names)
        if (cfg.check_imports) {
            try {
                // Check require() calls on RAW code (module name is in string)
                std::regex require_re("require\\s*\\(\\s*['\"]([^'\"]+)['\"]\\s*\\)",
                                      std::regex::ECMAScript);
                auto begin = std::sregex_iterator(code.begin(), code.end(), require_re);
                auto end_it = std::sregex_iterator();
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].str();
                    // Strip node: prefix
                    if (mod.size() > 5 && mod.substr(0, 5) == "node:") mod = mod.substr(5);
                    // Skip relative imports
                    if (!mod.empty() && (mod[0] == '.' || mod[0] == '/')) continue;
                    // Strip subpath: fs/promises → fs
                    auto slash = mod.find('/');
                    if (slash != std::string::npos && (mod.empty() || mod[0] != '@'))
                        mod = mod.substr(0, slash);
                    if (JS_KNOWN_MODULES.find(mod) == JS_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown module '{}' in JavaScript polyglot block", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "NAAb runs JavaScript via QuickJS (embedded), not Node.js.\n"
                                "  npm packages, require(), and Node.js builtins are NOT available.\n"
                                "  Use NAAb stdlib modules instead:\n"
                                "    File I/O: use file -> file.read(), file.write()\n"
                                "    HTTP:     use http -> http.get(), http.post()\n"
                                "    JSON:     JSON.parse() / JSON.stringify() are built in to QuickJS\n"
                                "  Rewrite the logic using NAAb stdlib or pure JS (no external modules).",
                                fmt::format("const x = require('{}')", mod),
                                "const result = JSON.stringify(data)  // use built-in QuickJS APIs"));
                    }
                }

                // Check ES import ... from '...' statements on RAW code
                std::regex import_re("from\\s+['\"]([^'\"]+)['\"]",
                                     std::regex::ECMAScript);
                begin = std::sregex_iterator(code.begin(), code.end(), import_re);
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].str();
                    if (mod.size() > 5 && mod.substr(0, 5) == "node:") mod = mod.substr(5);
                    if (!mod.empty() && (mod[0] == '.' || mod[0] == '/')) continue;
                    auto slash = mod.find('/');
                    if (slash != std::string::npos && (mod.empty() || mod[0] != '@'))
                        mod = mod.substr(0, slash);
                    if (JS_KNOWN_MODULES.find(mod) == JS_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown module '{}' in JavaScript polyglot block", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "NAAb runs JavaScript via QuickJS (embedded), not Node.js.\n"
                                "  npm packages, require(), and Node.js builtins are NOT available.\n"
                                "  Use NAAb stdlib modules instead:\n"
                                "    File I/O: use file -> file.read(), file.write()\n"
                                "    HTTP:     use http -> http.get(), http.post()\n"
                                "    JSON:     JSON.parse() / JSON.stringify() are built in to QuickJS\n"
                                "  Rewrite the logic using NAAb stdlib or pure JS (no external modules).",
                                fmt::format("import x from '{}'", mod),
                                "const result = JSON.stringify(data)  // use built-in QuickJS APIs"));
                    }
                }
            } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
        }

        // 2. API signature checks (cleaned + raw for string-literal patterns)
        if (cfg.check_api_signatures) {
            std::string err = runChecks(JS_API_CHECKS);
            if (!err.empty()) return err;
            err = runChecksRaw(JS_API_RAW_CHECKS);
            if (!err.empty()) return err;
        }

        // 3. Dangerous patterns (raw code — patterns include backtick/quote chars)
        if (cfg.check_dangerous_eval) {
            std::string err = runChecksRaw(JS_EVAL_CHECKS);
            if (!err.empty()) return err;
        }
    }

    // === SHELL ===
    else if (language == "shell" || language == "bash" || language == "sh") {
        // 1. Syntax validation
        if (cfg.check_shell_syntax) {
            std::string err = runChecks(SHELL_SYNTAX_CHECKS);
            if (!err.empty()) return err;
        }

        // 2. Command typo detection
        if (cfg.check_imports) {
            // Extract first word of each line in the cleaned code
            std::istringstream stream(clean);
            std::string cmd_line;
            while (std::getline(stream, cmd_line)) {
                // ltrim
                size_t start = cmd_line.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                std::string trimmed = cmd_line.substr(start);
                if (trimmed.empty() || trimmed[0] == '#') continue;
                // Skip variable assignments (VAR=value), pipes, subshells
                size_t eq_pos = trimmed.find('=');
                size_t sp_pos = trimmed.find_first_of(" \t");
                if (eq_pos != std::string::npos && (sp_pos == std::string::npos || eq_pos < sp_pos))
                    continue;
                // Skip lines starting with control chars
                if (trimmed[0] == '|' || trimmed[0] == '&' || trimmed[0] == ';' ||
                    trimmed[0] == '(' || trimmed[0] == ')' || trimmed[0] == '{' ||
                    trimmed[0] == '}' || trimmed[0] == '!' || trimmed[0] == '$')
                    continue;
                // Extract first word
                size_t word_end = trimmed.find_first_of(" \t;|&<>()");
                std::string cmd = (word_end != std::string::npos)
                    ? trimmed.substr(0, word_end) : trimmed;
                if (cmd.empty()) continue;
                // Skip paths (/usr/bin/foo, ./script)
                if (cmd.find('/') != std::string::npos || cmd.find('.') == 0) continue;

                // Check known typos first
                auto typo_it = SHELL_COMMAND_TYPOS.find(cmd);
                if (typo_it != SHELL_COMMAND_TYPOS.end()) {
                    return enforce("code_quality.semantic_checks", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Unknown command '{}' — did you mean '{}'?",
                                cmd, typo_it->second),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.semantic_checks",
                            fmt::format("Replace '{}' with '{}'", cmd, typo_it->second), "", ""));
                }
            }
        }

        // 3. Safety checks
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(SHELL_SAFETY_CHECKS);
            if (!err.empty()) return err;
            err = runChecks(SHELL_UNQUOTED_VAR_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === GO ===
    else if (language == "go" || language == "golang") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(GO_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === RUST ===
    else if (language == "rust") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(RUST_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === C++ ===
    else if (language == "cpp" || language == "c++") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(CPP_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === C# ===
    else if (language == "csharp" || language == "cs") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(CSHARP_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === RUBY ===
    else if (language == "ruby" || language == "rb") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(RUBY_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === PHP ===
    else if (language == "php") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(PHP_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }
    // === NIM ===
    else if (language == "nim") {
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(NIM_SAFETY_CHECKS);
            if (!err.empty()) return err;
        }
    }

    recordPass("code_quality.semantic_checks", cfg.level);
    return "";
}

// --- Security: Shell Injection ---
std::string GovernanceEngine::checkShellInjection(const std::string& code, int line) {
    auto& cfg = rules().restrictions.shell_injection;
    if (!cfg.enabled) return "";
    clearTrace();
    static const std::vector<std::string> default_patterns = {
        // Original: pipe-to-shell and chmod
        "curl.*\\|\\s*(?:sh|bash)", "wget.*\\|\\s*(?:sh|bash)",
        "eval\\s+\\$", "\\$\\(curl", "\\$\\(wget",
        "bash\\s+-c.*\\$", "chmod\\s+777", "chmod\\s+\\+x.*\\$",
        // Reverse shells
        "bash\\s+-i\\s+>&\\s*/dev/tcp/",
        "nc\\s+.*-e\\s+/bin/(?:sh|bash)",
        "ncat\\s+.*-e\\s+/bin/(?:sh|bash)",
        // Encoded command execution
        "base64\\s+-d\\s*\\|\\s*(?:sh|bash)",
        "python[23]?\\s+-c.*exec\\s*\\(",
        // Scheduled persistence
        "crontab\\s+-[erl]",
        "\\bat\\s+\\d{1,2}:\\d{2}",
        // Environment manipulation
        "export\\s+PATH\\s*=",
        "LD_PRELOAD\\s*=",
        "LD_LIBRARY_PATH\\s*=",
        // File descriptor tricks
        "exec\\s+\\d+<>/dev/tcp/",
        // Pipe to interpreter
        "\\|\\s*python[23]?\\s",
    };
    auto& pats = cfg.patterns.empty() ? default_patterns : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.shell_injection", cfg.level,
            formatError(cfg.level, fmt::format("Shell injection pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.shell_injection",
                "Shell injection detected — user-controlled input reaches shell execution.\n"
                "Never pipe variables into shell commands or use eval with dynamic strings.",
                "cmd = \"grep \" + user_input + \" /etc/passwd\"\nos.system(cmd)",
                "import shlex\ncmd = [\"grep\", shlex.quote(user_input), \"data/safe.txt\"]\nsubprocess.run(cmd, shell=False)"));
    }
    recordPass("restrictions.shell_injection", cfg.level);
    return "";
}

// --- Security: Code Injection ---
std::string GovernanceEngine::checkCodeInjection(const std::string& language,
                                                  const std::string& code,
                                                  const std::string& normalized_code,
                                                  int line) {
    auto& cfg = rules().restrictions.code_injection;
    if (!cfg.enabled) return "";
    clearTrace();
    // Case-sensitive patterns: API names are exact (eval, exec, Function, os.system, etc.)
    std::vector<std::string> pats;
    // Case-insensitive patterns: SQL keywords (SELECT/select, INSERT/insert, etc.)
    std::vector<std::string> pats_ci;
    if (cfg.block_dynamic_code_gen) { pats.push_back("\\beval\\s*\\("); pats.push_back("\\bexec\\s*\\("); pats.push_back("\\bFunction\\s*\\("); }
    if (cfg.block_sql_injection_patterns) {
        pats_ci.push_back("(?:SELECT|INSERT|UPDATE|DELETE)\\s+.*['\"]\\s*\\+");
        pats_ci.push_back("f['\"].*(?:SELECT|INSERT|UPDATE|DELETE).*\\{");
    }
    if (cfg.block_command_injection) {
        if (language == "python") {
            pats.push_back("os\\.system\\s*\\(");
            pats.push_back("subprocess\\.(?:call|Popen|run|check_output|check_call)\\s*\\(");
            pats.push_back("ctypes\\.(?:CDLL|cdll)\\s*\\(");
            // Block dangerous imports (bare import grants access to system calls)
            pats.push_back("\\bimport\\s+(?:os|subprocess|shutil|ctypes|pty|commands)\\b");
            pats.push_back("\\bfrom\\s+(?:os|subprocess|shutil|ctypes|pty|commands)\\b");
        } else if (language == "go" || language == "golang") {
            pats.push_back("exec\\.Command\\s*\\(");
        } else if (language == "rust") {
            pats.push_back("Command::new\\s*\\(");
        } else if (language == "cpp" || language == "c++") {
            pats.push_back("\\bsystem\\s*\\(");
            pats.push_back("\\bpopen\\s*\\(");
        } else if (language == "csharp" || language == "cs") {
            pats.push_back("Process\\.Start\\s*\\(");
        } else if (language == "ruby" || language == "rb") {
            pats.push_back("\\bsystem\\s*\\(");
        } else if (language == "php") {
            pats.push_back("\\bsystem\\s*\\(");
            pats.push_back("\\bexec\\s*\\(");
            pats.push_back("\\bshell_exec\\s*\\(");
            pats.push_back("\\bpassthru\\s*\\(");
        } else if (language == "nim") {
            pats.push_back("\\bexecProcess\\s*\\(");
            pats.push_back("\\bstartProcess\\s*\\(");
        } else if (language == "javascript" || language == "typescript") {
            // Qualified child_process method calls
            pats.push_back("child_process\\.(?:exec|execSync|execFile|execFileSync|spawn|spawnSync|fork)\\s*\\(");
            // Destructured child_process functions (names unique to child_process)
            pats.push_back("\\bexecSync\\s*\\(");
            pats.push_back("\\bexecFile(?:Sync)?\\s*\\(");
            pats.push_back("\\bspawnSync\\s*\\(");
            // Deno/Bun command execution
            pats.push_back("Deno\\.(?:run|Command)\\s*\\(");
            pats.push_back("Bun\\.(?:spawn|spawnSync)\\s*\\(");
            // Note: require('child_process') and import 'child_process' are handled
            // by checkImports() which uses normalized (non-stripped) code.
            // exec() is already caught by block_dynamic_code_gen globally.
        } else {
            // Fallback for unknown languages
            pats.push_back("os\\.system\\s*\\(");
            pats.push_back("subprocess\\.(?:call|Popen|run|check_output|check_call)\\s*\\(");
        }
    }
    // API names are case-sensitive on stripped code (Function !== function, os.system !== OS.SYSTEM)
    std::string found = searchPatterns(code, pats, false);
    // SQL patterns are case-insensitive on normalized code — they match string contents
    // (e.g., f"SELECT {var}", "SELECT " + var) which are removed by stripStringLiterals
    if (found.empty()) found = searchPatterns(normalized_code, pats_ci, true);
    if (!found.empty()) {
        return enforce("restrictions.code_injection", cfg.level,
            formatError(cfg.level, fmt::format("Code injection pattern in {} block: \"{}\"", language, found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.code_injection",
                "Dynamic code execution detected — eval/exec/Function can run arbitrary code.\n"
                "Use data-driven approaches (lookup tables, config) instead of code generation.",
                "result = eval(user_expression)",
                "ops = {\"add\": fn(a,b) { a+b }, \"mul\": fn(a,b) { a*b }}\nresult = ops.get(op_name)(a, b)"));
    }
    recordPass("restrictions.code_injection", cfg.level);
    return "";
}

// --- Security: Privilege Escalation ---
std::string GovernanceEngine::checkPrivilegeEscalation(const std::string& code, int line) {
    auto& cfg = rules().restrictions.privilege_escalation;
    if (!cfg.enabled) return "";
    clearTrace();
    std::vector<std::string> pats;
    if (cfg.block_sudo) pats.push_back("\\bsudo\\s");
    if (cfg.block_su) pats.push_back("\\bsu\\s+-");
    if (cfg.block_chmod_suid) pats.push_back("chmod\\s+[ugo]*s");
    if (cfg.block_setuid) pats.push_back("\\bsetuid\\b");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.privilege_escalation", cfg.level,
            formatError(cfg.level, fmt::format("Privilege escalation: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.privilege_escalation",
                "Privilege escalation commands are blocked in polyglot blocks.\n"
                "NAAb scripts run with the invoking user's permissions — no elevation allowed.",
                "os.system(\"sudo chmod 777 /etc/config\")",
                "file.write(\"./output/config.txt\", data)  // write to allowed paths only"));
    }
    recordPass("restrictions.privilege_escalation", cfg.level);
    return "";
}

// --- Security: Data Exfiltration ---
std::string GovernanceEngine::checkDataExfiltration(const std::string& code, int line) {
    auto& cfg = rules().restrictions.data_exfiltration;
    if (!cfg.enabled) return "";
    clearTrace();

    // If user provided custom patterns, use those exclusively
    if (!cfg.patterns.empty()) {
        std::string found = searchPatterns(code, cfg.patterns);
        if (!found.empty()) {
            return enforce("restrictions.data_exfiltration", cfg.level,
                formatError(cfg.level, "Data exfiltration pattern detected",
                    line > 0 ? fmt::format("line {}", line) : "", "restrictions.data_exfiltration",
                    "Data exfiltration detected — secrets or data are being prepared for external transmission.\n"
                    "Process data locally; do not encode, transmit, or stage secrets for export.",
                    "requests.post(url, data=secret)",
                    "// Use secrets in-place, don't send them externally\nheaders = {\"Authorization\": env.get(\"API_KEY\")}"));
        }
        recordPass("restrictions.data_exfiltration", cfg.level);
        return "";
    }

    // Default patterns — built from enabled categories
    std::vector<std::string> pats;

    // Encoding-based exfil (original 2 patterns, relaxed keyword requirement)
    if (cfg.block_base64_encode_secrets) {
        pats.push_back("base64\\.(?:b64encode|encodebytes|b64decode)\\s*\\(");
        pats.push_back("base64\\.(?:b64encode|encode).*(?:password|secret|key|token)");
    }
    if (cfg.block_hex_encode_secrets) {
        pats.push_back("binascii\\.(?:hexlify|unhexlify)\\s*\\(");
        pats.push_back("\\.hex\\(\\).*(?:password|secret|key|token)");
    }
    if (cfg.block_encoding_chains) {
        pats.push_back("codecs\\.(?:encode|decode)\\s*\\(");
        pats.push_back("\\.encode\\(.*\\)\\.decode\\(");
    }

    // Network exfiltration
    if (cfg.block_network_exfil) {
        pats.push_back("requests\\.(?:post|put|patch)\\s*\\(");
        pats.push_back("urllib\\.request\\.(?:urlopen|Request)\\s*\\(");
        pats.push_back("http\\.client\\.HTTP(?:S)?Connection\\s*\\(");
        pats.push_back("fetch\\s*\\(");
        pats.push_back("axios\\.(?:post|put|patch)\\s*\\(");
        pats.push_back("new\\s+XMLHttpRequest");
        pats.push_back("httpx\\.(?:post|put|patch|AsyncClient)\\s*\\(");
    }

    // Raw socket exfiltration
    if (cfg.block_socket_exfil) {
        pats.push_back("socket\\.(?:connect|create_connection)\\s*\\(");
        pats.push_back("socket\\.socket\\s*\\(");
    }

    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.data_exfiltration", cfg.level,
            formatError(cfg.level, "Data exfiltration pattern detected",
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.data_exfiltration",
                "Data exfiltration detected — secrets or data are being prepared for external transmission.\n"
                "Process data locally; do not encode, transmit, or stage secrets for export.",
                "requests.post(url, data=secret)",
                "// Use secrets in-place, don't send them externally\nheaders = {\"Authorization\": env.get(\"API_KEY\")}"));
    }
    recordPass("restrictions.data_exfiltration", cfg.level);
    return "";
}

// --- Security: Resource Abuse ---
std::string GovernanceEngine::checkResourceAbuse(const std::string& code, int line) {
    auto& cfg = rules().restrictions.resource_abuse;
    if (!cfg.enabled) return "";
    clearTrace();
    std::vector<std::string> pats;
    if (cfg.block_fork_bomb) { pats.push_back(":\\(\\)\\{\\s*:\\|:&\\s*\\};:"); pats.push_back("fork\\(\\).*fork\\(\\)"); }
    if (cfg.block_disk_filling) pats.push_back("dd\\s+if=/dev/zero");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.resource_abuse", cfg.level,
            formatError(cfg.level, fmt::format("Resource abuse pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.resource_abuse",
                "This pattern could cause resource exhaustion (fork bombs, disk filling).\n"
                "Use bounded operations with explicit limits.",
                ":(){ :|:& };:  // fork bomb",
                "for i in 0..10 { process(items[i]) }  // bounded iteration"));
    }
    recordPass("restrictions.resource_abuse", cfg.level);
    return "";
}

// --- Security: Info Disclosure ---
std::string GovernanceEngine::checkInfoDisclosure(const std::string& language,
                                                   const std::string& code, int line) {
    auto& cfg = rules().restrictions.information_disclosure;
    if (!cfg.enabled) return "";
    clearTrace();
    std::vector<std::string> pats;
    if (cfg.block_env_dump) {
        pats.push_back("os\\.environ(?!\\[)");
        pats.push_back("process\\.env(?!\\.)");
        // Only add bare "env" pattern for languages where it means "dump env"
        // Skip for NAAb where "env" is a safe stdlib module (env.get_args, env.get)
        if (language != "naab") {
            pats.push_back("\\benv\\b(?!\\.)");
        }
    }
    if (cfg.block_process_listing) { pats.push_back("ps\\s+aux"); pats.push_back("ps\\s+-ef"); }
    if (cfg.block_system_info_leak) { pats.push_back("uname\\s+-a"); pats.push_back("cat\\s+/etc/passwd"); }
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.information_disclosure", cfg.level,
            formatError(cfg.level, fmt::format("Information disclosure pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.information_disclosure",
                "Dumping system environment or process information is blocked.\n"
                "Access specific environment variables by name instead of enumerating all.",
                "for k, v in os.environ.items():\n    print(k, v)",
                "let db_host = env.get(\"DB_HOST\")  // access specific vars only"));
    }
    recordPass("restrictions.information_disclosure", cfg.level);
    return "";
}

// --- Security: Crypto Weakness ---
std::string GovernanceEngine::checkCryptoWeakness(const std::string& code,
                                                   const std::string& normalized_code,
                                                   int line) {
    auto& cfg = rules().restrictions.crypto;
    if (!cfg.enabled) return "";
    clearTrace();
    std::vector<std::string> pats;
    if (cfg.block_weak_hashing) {
        auto& hashes = cfg.weak_hashes.empty() ? (const std::vector<std::string>&)(std::vector<std::string>{"md5", "sha1"}) : cfg.weak_hashes;
        for (const auto& h : hashes) { pats.push_back("\\b" + h + "\\b"); pats.push_back("hashlib\\." + h); }
    }
    if (cfg.block_weak_encryption) {
        auto& ciphers = cfg.weak_ciphers.empty() ? (const std::vector<std::string>&)(std::vector<std::string>{"des", "rc4", "blowfish"}) : cfg.weak_ciphers;
        for (const auto& c : ciphers) pats.push_back("\\b" + c + "\\b");
    }
    // Weak hash/cipher patterns search stripped code (avoid false positives from strings)
    std::string found = searchPatterns(code, pats);
    // Hardcoded key pattern needs normalized code — quotes are stripped out
    if (found.empty() && cfg.block_hardcoded_keys) {
        std::vector<std::string> key_pats = {"(?:encryption|signing|crypto)_key\\s*=\\s*['\"][^'\"]+['\"]"};
        found = searchPatterns(normalized_code, key_pats);
    }
    if (!found.empty()) {
        return enforce("restrictions.crypto", cfg.level,
            formatError(cfg.level, fmt::format("Cryptographic weakness: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.crypto",
                "Use strong cryptographic algorithms (SHA-256+, AES-256).\n"
                "Weak algorithms like MD5 and SHA-1 are vulnerable to collision attacks.",
                "hash = hashlib.md5(data)  // weak",
                "hash = hashlib.sha256(data)  // strong"));
    }
    recordPass("restrictions.crypto", cfg.level);
    return "";
}

// --- Security: VCS Secret Extraction (co-occurrence detection) ---
// Gap #12: Detects attempts to extract secrets from version control history.
// Uses signal co-occurrence instead of hard patterns — resilient to rephrasing.
//
// Signal 1: VCS invocation (git, .git, subprocess calling git, etc.)
// Signal 2: History traversal (log, show, rev-list, reflog, diff, blame)
// Signal 3: Content search/extraction (-S, -G, -p, --format, grep, pipe)
// Signal 4: Secret-adjacent targets (key, token, secret, password, .pem, .env)
//
// 3/4 signals = ADVISORY, 4/4 = configured level (default SOFT)
std::string GovernanceEngine::checkVcsSecretExtraction(const std::string& code, int line) {
    auto& cfg = rules().restrictions.vcs_secret_extraction;
    if (!cfg.enabled) return "";
    clearTrace();

    // Lowercase for case-insensitive matching
    std::string lower = code;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Signal 1: VCS invocation
    bool has_vcs = false;
    static const std::vector<std::string> vcs_signals = {
        "git ", "git\\b", "\\.git", "subprocess.*git", "exec.*git",
        "os\\.system.*git", "os\\.popen.*git", "run.*git",
        "system(\"git", "system('git", "`git ", "$(git ",
        "svn ", "hg ", "mercurial"
    };
    for (const auto& pat : vcs_signals) {
        try {
            if (std::regex_search(lower, std::regex(pat))) { has_vcs = true; break; }
        } catch (...) {
            if (lower.find(pat) != std::string::npos) { has_vcs = true; break; }
        }
    }

    // Signal 2: History traversal intent
    bool has_history = false;
    static const std::vector<std::string> history_signals = {
        "\\blog\\b", "\\bshow\\b", "\\bblame\\b", "\\breflog\\b",
        "rev-list", "rev-parse", "\\bdiff\\b", "cat-file",
        "fsck", "for-each-ref", "\\bhistory\\b", "\\bcommit\\b"
    };
    for (const auto& pat : history_signals) {
        try {
            if (std::regex_search(lower, std::regex(pat))) { has_history = true; break; }
        } catch (...) {
            if (lower.find(pat) != std::string::npos) { has_history = true; break; }
        }
    }

    // Signal 3: Content search/extraction
    bool has_search = false;
    static const std::vector<std::string> search_signals = {
        "-s\\b", "-g\\b", "-p\\b", "--format", "--pretty",
        "\\bgrep\\b", "\\bsed\\b", "\\bawk\\b", "\\bxargs\\b",
        "\\bpipe\\b", "\\b>\\s", ">>\\s", "tee\\b",
        "\\bread\\b", "open\\(", "write\\(", "extract",
        "--all", "--follow", "--source"
    };
    for (const auto& pat : search_signals) {
        try {
            if (std::regex_search(lower, std::regex(pat))) { has_search = true; break; }
        } catch (...) {
            if (lower.find(pat) != std::string::npos) { has_search = true; break; }
        }
    }

    // Signal 4: Secret-adjacent targets
    bool has_secret = false;
    static const std::vector<std::string> secret_signals = {
        "\\bkey\\b", "\\btoken\\b", "\\bsecret\\b", "\\bpassword\\b",
        "\\bcredential", "\\bprivate", "\\.pem\\b", "\\.key\\b",
        "\\.env\\b", "api.?key", "auth.?token", "access.?key",
        "signing.?key", "\\bcert\\b", "\\bpkcs", "\\baws_",
        "\\bazure", "\\bgcp_", "service.?account"
    };
    for (const auto& pat : secret_signals) {
        try {
            if (std::regex_search(lower, std::regex(pat))) { has_secret = true; break; }
        } catch (...) {
            if (lower.find(pat) != std::string::npos) { has_secret = true; break; }
        }
    }

    int signal_count = (has_vcs ? 1 : 0) + (has_history ? 1 : 0) +
                       (has_search ? 1 : 0) + (has_secret ? 1 : 0);

    // VCS invocation is a prerequisite — without it, other signals are just normal code
    if (!has_vcs) {
        recordPass("restrictions.vcs_secret_extraction", cfg.level);
        return "";
    }

    if (signal_count >= 4) {
        // Full co-occurrence: all 4 signals present
        return enforce("restrictions.vcs_secret_extraction", cfg.level,
            formatError(cfg.level,
                "VCS secret extraction: code combines version control access, "
                "history traversal, content extraction, and secret-related targets "
                "(4/4 signals detected)",
                line > 0 ? fmt::format("line {}", line) : "",
                "restrictions.vcs_secret_extraction",
                "Do not extract secrets from version control history.\n"
                "If you need credentials, use environment variables or a secrets manager.",
                "git log -p | grep -i 'password\\|secret\\|key'",
                "let api_key = env.get(\"API_KEY\")  // use env vars for secrets"));
    } else if (signal_count == 3 && has_secret) {
        // 3/4 with VCS + secrets = advisory (likely targeted extraction)
        std::string missing;
        if (!has_history) missing = "history-traversal";
        else missing = "content-extraction";
        return enforce("restrictions.vcs_secret_extraction", EnforcementLevel::ADVISORY,
            formatError(EnforcementLevel::ADVISORY,
                fmt::format("Possible VCS secret extraction: 3/4 co-occurrence signals "
                    "detected (missing: {})", missing),
                line > 0 ? fmt::format("line {}", line) : "",
                "restrictions.vcs_secret_extraction",
                "This code pattern resembles secret extraction from version control.\n"
                "If you need credentials, use environment variables instead.", "", ""));
    }

    recordPass("restrictions.vcs_secret_extraction", cfg.level);
    return "";
}

// --- Security: Obfuscation Signal Co-occurrence Detection ---
// Detects multi-statement obfuscation chains (e.g., base64 decode → exec,
// chr() concatenation → __import__, globals()['__builtins__'] chains).
// Uses signal co-occurrence (same approach as checkVcsSecretExtraction):
// Signal 1 (exec sink) + Signal 2 (encoding) + Signal 3 (dynamic access) + Signal 4 (indirection)
// Scoring: sink + 1 other = ADVISORY, sink + 2 others = configured level
std::string GovernanceEngine::checkObfuscationSignals(
    const std::string& language, const std::string& raw_code,
    const std::string& stripped_code, int line) {
    auto& cfg = rules().restrictions.obfuscation;
    if (!cfg.enabled) return "";
    clearTrace();

    // Helper: check if any pattern matches
    auto has_signal = [](const std::string& code, const std::vector<std::string>& patterns) -> bool {
        for (const auto& pat : patterns) {
            try {
                if (std::regex_search(code, std::regex(pat))) return true;
            } catch (...) {
                if (code.find(pat) != std::string::npos) return true;
            }
        }
        return false;
    };

    bool has_sink = false, has_encoding = false, has_dynamic = false, has_indirect = false;

    if (language == "python") {
        // Signal 1: Execution sinks
        has_sink = has_signal(stripped_code, {
            "\\bexec\\s*\\(", "\\beval\\s*\\(", "\\bcompile\\s*\\("
        });

        // Signal 2: Payload encoding/reconstruction (use raw_code — encoded payloads live in strings)
        has_encoding = has_signal(raw_code, {
            "base64\\.b64decode", "codecs\\.decode",
            "chr\\s*\\(\\s*\\d+\\s*\\)\\s*\\+",
            "bytearray\\(", "binascii\\.unhexlify"
        });

        // Signal 3: Dynamic import/attribute access
        has_dynamic = has_signal(stripped_code, {
            "globals\\s*\\(\\s*\\)\\s*\\[", "__builtins__",
            "__import__", "__loader__", "types\\.ModuleType",
            "__subclasses__"
        });

        // Signal 4: Indirection mechanisms
        has_indirect = has_signal(stripped_code, {
            "ctypes\\.(?:CDLL|cdll)", "setattr\\s*\\(",
            "__mro__", "__dict__\\s*\\["
        });
    } else if (language == "javascript" || language == "js") {
        has_sink = has_signal(stripped_code, {
            "\\beval\\s*\\(", "\\bFunction\\s*\\(",
            "setTimeout\\s*\\(", "setInterval\\s*\\(",
            "Reflect\\.apply\\s*\\(\\s*eval"
        });
        has_encoding = has_signal(raw_code, {
            "\\batob\\s*\\(", "String\\.fromCharCode",
            "decodeURIComponent", "\\bunescape\\s*\\("
        });
        has_dynamic = has_signal(stripped_code, {
            "Reflect\\.", "Proxy\\s*\\(", "constructor\\s*\\["
        });
        has_indirect = false;  // JS has 3 signal groups
    } else if (language == "ruby") {
        has_sink = has_signal(stripped_code, {
            "\\beval\\s*\\(", "\\bsend\\s*\\(", "\\binstance_eval\\b",
            "\\bclass_eval\\b", "\\bbinding\\.eval\\b"
        });
        has_encoding = has_signal(raw_code, {
            "Base64\\.decode64", "\\bpack\\s*\\(", "\\bunpack\\s*\\("
        });
        has_dynamic = has_signal(stripped_code, {
            "\\bmethod_missing\\b", "\\bdefine_method\\b",
            "\\bconst_get\\s*\\(", "\\bObject\\.send\\s*\\("
        });
        has_indirect = has_signal(stripped_code, {
            "\\bsystem\\s*\\(", "\\bexec\\s*\\(", "\\b`.*`"
        });
    } else if (language == "php") {
        has_sink = has_signal(stripped_code, {
            "\\beval\\s*\\(", "\\bassert\\s*\\(",
            "\\bcreate_function\\s*\\("
        });
        // preg_replace /e modifier is inside a string literal — check raw_code
        if (!has_sink) {
            has_sink = has_signal(raw_code, {
                "\\bpreg_replace\\s*\\(.*/e"
            });
        }
        has_encoding = has_signal(raw_code, {
            "\\bbase64_decode\\s*\\(", "\\bhex2bin\\s*\\(",
            "\\bgzinflate\\s*\\(", "\\bstr_rot13\\s*\\("
        });
        has_dynamic = has_signal(stripped_code, {
            "\\bcall_user_func\\s*\\(", "\\$\\$",
            "\\bcall_user_func_array\\s*\\("
        });
        has_indirect = has_signal(stripped_code, {
            "\\bReflectionFunction\\b", "\\bReflectionMethod\\b"
        });
    } else if (language == "shell" || language == "sh" || language == "bash") {
        has_sink = has_signal(stripped_code, {
            "\\beval\\s", "\\bexec\\s", "\\bsource\\s",
            "\\$\\(.*\\$\\("  // nested command substitution
        });
        has_encoding = has_signal(raw_code, {
            "\\\\x[0-9a-fA-F]{2}", "\\$'\\\\",
            "xxd\\s+-r", "base64\\s+-d"
        });
        has_dynamic = has_signal(stripped_code, {
            "\\$\\{!",  // variable indirection
            "printf\\s+.*\\\\x"
        });
        has_indirect = false;
    } else if (language == "go" || language == "golang") {
        has_sink = has_signal(stripped_code, {
            "reflect\\.Value.*\\.Call\\s*\\(", "plugin\\.Open\\s*\\(",
            "exec\\.Command\\s*\\("
        });
        has_encoding = has_signal(raw_code, {
            "encoding/hex", "encoding/base64"
        });
        has_dynamic = has_signal(stripped_code, {
            "reflect\\.(?:ValueOf|TypeOf)\\s*\\(",
            "unsafe\\.Pointer\\s*\\(",
            "//go:linkname"
        });
        has_indirect = false;
    } else if (language == "csharp" || language == "cs") {
        has_sink = has_signal(stripped_code, {
            "Assembly\\.Load(?:From)?\\s*\\(",
            "Activator\\.CreateInstance\\s*\\(",
            "DynamicInvoke\\s*\\("
        });
        has_encoding = has_signal(raw_code, {
            "Convert\\.FromBase64String",
            "Encoding\\.(?:UTF8|ASCII)\\.GetString"
        });
        has_dynamic = has_signal(stripped_code, {
            "Type\\.GetType\\s*\\(", "MethodInfo\\.Invoke\\s*\\(",
            "Marshal\\.(?:Copy|PtrToStructure)\\s*\\("
        });
        has_indirect = false;
    } else if (language == "rust") {
        has_sink = has_signal(stripped_code, {
            "std::process::Command", "\\bunsafe\\s*\\{"
        });
        has_encoding = has_signal(raw_code, {
            "base64::decode", "hex::decode"
        });
        has_dynamic = has_signal(stripped_code, {
            "\\btransmute\\s*[:<(]", "\\bas\\s+\\*(?:const|mut)\\b"
        });
        has_indirect = false;
    } else {
        // Unknown languages: skip co-occurrence
        recordPass("restrictions.obfuscation", cfg.level);
        return "";
    }

    // Count signals present
    int signal_count = (has_sink ? 1 : 0) + (has_encoding ? 1 : 0)
                     + (has_dynamic ? 1 : 0) + (has_indirect ? 1 : 0);

    // Build description of what was detected
    if (has_sink && signal_count >= 3) {
        // Sink + 2 or more other signals = configured enforcement level
        std::string signals;
        if (has_encoding) signals += "payload encoding, ";
        if (has_dynamic) signals += "dynamic access, ";
        if (has_indirect) signals += "indirection, ";
        if (!signals.empty()) signals = signals.substr(0, signals.size() - 2);

        addTrace(fmt::format("Obfuscation co-occurrence: exec sink + {} ({} signals total)",
                             signals, signal_count));
        return enforce("restrictions.obfuscation", cfg.level,
            formatError(cfg.level,
                fmt::format("Obfuscation pattern in {} block: execution sink combined with {}",
                            language, signals),
                line > 0 ? fmt::format("line {}", line) : "",
                "restrictions.obfuscation",
                "Multiple obfuscation signals detected in the same code block.\n"
                "This pattern resembles an attempt to bypass import or execution restrictions.\n"
                "Use standard import statements and direct function calls.",
                "", ""));
    } else if (has_sink && signal_count == 2) {
        // Sink + 1 other signal = ADVISORY warning
        std::string signal_name = has_encoding ? "payload encoding"
                                : has_dynamic ? "dynamic access"
                                : "indirection";
        addTrace(fmt::format("Obfuscation advisory: exec sink + {}", signal_name));
        return enforce("restrictions.obfuscation", EnforcementLevel::ADVISORY,
            formatError(EnforcementLevel::ADVISORY,
                fmt::format("Potential obfuscation in {} block: execution sink combined with {}",
                            language, signal_name),
                line > 0 ? fmt::format("line {}", line) : "",
                "restrictions.obfuscation",
                "This code combines dynamic execution with encoding or indirect access.\n"
                "While it may be legitimate, this pattern is commonly used for obfuscation.",
                "", ""));
    }

    recordPass("restrictions.obfuscation", cfg.level);
    return "";
}

// --- Per-Language: Imports ---
std::string GovernanceEngine::checkImports(const std::string& language,
                                            const std::string& code, int line) {
    auto& cfg = rules().restrictions.imports;

    // Check per-language config even if restrictions.imports is not enabled
    auto it = rules().languages.per_language.find(language);
    bool has_per_lang = (it != rules().languages.per_language.end() &&
                         (!it->second.imports.blocked.empty() || !it->second.banned_imports.empty()));
    if (!cfg.enabled && !has_per_lang) return "";
    clearTrace();

    // Build import patterns for this language
    std::vector<std::string> blocked;
    if (cfg.enabled) {
        if (cfg.blocked.count(language)) blocked = cfg.blocked.at(language);
        if (cfg.blocked.count("any")) {
            auto& any = cfg.blocked.at("any");
            blocked.insert(blocked.end(), any.begin(), any.end());
        }
    }

    // Also check per-language config
    if (it != rules().languages.per_language.end()) {
        auto& lc = it->second;
        for (const auto& b : lc.imports.blocked) blocked.push_back(b);
        for (const auto& b : lc.banned_imports) blocked.push_back(b);
    }

    for (const auto& imp : blocked) {
        std::string pat;
        if (language == "python") pat = "(?:import\\s+" + imp + "|from\\s+" + imp + ")";
        else if (language == "javascript") pat = "(?:require\\s*\\(\\s*['\"]" + imp + "['\"]|import.*from\\s*['\"]" + imp + "['\"])";
        else if (language == "go") pat = "\"" + imp + "\"";
        else if (language == "ruby") pat = "require\\s*['\"]" + imp + "['\"]";
        else pat = imp;

        try {
            std::regex re(pat, std::regex::icase);
            if (std::regex_search(code, re)) {
                auto level = cfg.enabled ? cfg.level : EnforcementLevel::HARD;
                // Suggest NAAb stdlib alternatives for commonly-blocked imports
                static const std::unordered_map<std::string, std::string> import_alternatives = {
                    {"os",         "NAAb alternatives: file.list_dir(), file.exists(), path.join(),\n"
                                   "  path.basename(), env.get(), process.run()"},
                    {"subprocess", "NAAb alternative: process.run(cmd, [args]) returns\n"
                                   "  {exit_code, stdout, stderr}"},
                    {"shutil",     "NAAb alternatives: file.copy(), file.move(), file.delete()"},
                    {"socket",     "NAAb has no raw socket API. Use the http module if available,\n"
                                   "  or process.run() to call curl/wget"},
                    {"ctypes",     "NAAb has no FFI. Use a polyglot block in the target language instead"},
                    {"pickle",     "NAAb alternative: json.stringify() / json.parse()"},
                    {"sys",        "NAAb alternatives: env.get_args() for CLI arguments,\n"
                                   "  process.exit(code) for sys.exit, env.get() for sys.platform"},
                };
                std::string help_text = fmt::format("The import \"{}\" is blocked by governance", imp);
                auto alt = import_alternatives.find(imp);
                if (alt != import_alternatives.end()) {
                    help_text += ".\n\n  " + alt->second;
                }
                return enforce("restrictions.imports", level,
                    formatError(level, fmt::format("Blocked import in {} block: \"{}\"", language, imp),
                        line > 0 ? fmt::format("line {}", line) : "",
                        cfg.enabled ? "restrictions.imports" : fmt::format("languages.per_language.{}.imports.blocked", language),
                        help_text, "", ""));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }
    recordPass("restrictions.imports", cfg.level);
    return "";
}

// --- Per-Language: Banned Functions ---
std::string GovernanceEngine::checkBannedFunctions(const std::string& language,
                                                    const std::string& code, int line) {
    auto it = rules().languages.per_language.find(language);
    if (it == rules().languages.per_language.end()) return "";
    auto& lc = it->second;
    if (lc.banned_functions.empty()) return "";
    clearTrace();

    for (const auto& func : lc.banned_functions) {
        try {
            // Escape regex metacharacters so entries like "curl |" are treated as literals
            std::string escaped;
            for (char c : func) {
                if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos)
                    escaped += '\\';
                escaped += c;
            }
            // For multi-word patterns, require word boundary or end-of-string after match
            // to prevent "rm -rf /" from matching "rm -rf /tmp/safe"
            if (func.find(' ') != std::string::npos) {
                escaped += "(?:\\s|$)";
            }
            std::regex re(escaped, std::regex::icase);
            if (std::regex_search(code, re)) {
                // Per-function alternatives so LLMs know what to use instead
                static const std::unordered_map<std::string, std::string> alternatives = {
                    {"eval(",     "Use a lookup table or if/else chain instead of dynamic evaluation"},
                    {"Function(", "Use a lookup table, switch/match, or define functions statically.\n"
                                  "  Example: instead of new Function('return ' + expr),\n"
                                  "  use a dict mapping: let ops = {\"add\": fn(a,b) { return a + b }}"},
                    {"exec(",     "Use file.read() + json.parse() for data, or import for code"},
                    {"compile(",  "Pre-define functions instead of compiling at runtime"},
                };
                auto alt_it = alternatives.find(func);
                std::string help = (alt_it != alternatives.end())
                    ? alt_it->second
                    : "This function is banned by governance policy";
                return enforce("languages.per_language.banned_functions", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Banned function in {} block: \"{}\"", language, func),
                        line > 0 ? fmt::format("line {}", line) : "",
                        fmt::format("languages.per_language.{}.banned_functions", language),
                        help, "", ""));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }
    return "";
}

// --- Per-Language: Style Rules ---
std::string GovernanceEngine::checkLanguageStyle(const std::string& language,
                                                  const std::string& code, int line) {
    auto it = rules().languages.per_language.find(language);
    if (it == rules().languages.per_language.end()) return "";
    auto& lc = it->second;
    clearTrace();

    // Shell: require set -e
    if (language == "shell" || language == "bash") {
        if (lc.require_set_e && code.find("set -e") == std::string::npos) {
            return enforce("languages.per_language.shell.require_set_e", lc.require_set_e_level,
                formatError(lc.require_set_e_level, "Shell block missing 'set -e'",
                    line > 0 ? fmt::format("line {}", line) : "",
                    "languages.per_language.shell.require_set_e",
                    "Add 'set -e' to exit on errors",
                    "echo \"hello\"",
                    "set -e\necho \"hello\""));
        }
    }

    // JS: no var
    if (language == "javascript" && lc.no_var) {
        try {
            std::regex re("\\bvar\\s+\\w");
            if (std::regex_search(code, re)) {
                return enforce("languages.per_language.javascript.no_var", lc.no_var_level,
                    formatError(lc.no_var_level, "Use 'let' or 'const' instead of 'var'",
                        line > 0 ? fmt::format("line {}", line) : "",
                        "languages.per_language.javascript.no_var",
                        "'var' has function scope — use 'let' or 'const' for block scope",
                        "var x = 1;", "let x = 1;  // or const x = 1;"));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }

    return "";
}

// --- Per-Language: Code Size ---
std::string GovernanceEngine::checkCodeSize(const std::string& language,
                                             const std::string& code, int line) {
    auto it = rules().languages.per_language.find(language);
    if (it == rules().languages.per_language.end()) return "";
    auto& lc = it->second;
    clearTrace();

    if (lc.max_lines > 0) {
        int lines = 1;
        for (char c : code) if (c == '\n') lines++;
        if (lines > lc.max_lines) {
            return enforce("languages.per_language.max_lines", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    fmt::format("{} block has {} lines (max: {})", language, lines, lc.max_lines),
                    line > 0 ? fmt::format("line {}", line) : "",
                    fmt::format("languages.per_language.{}.max_lines = {}", language, lc.max_lines),
                    "Break large blocks into smaller functions", "", ""));
        }
    }
    return "";
}

// --- Custom Rules ---
std::string GovernanceEngine::checkCustomRules(const std::string& language,
                                                const std::string& code, int line) {
    clearTrace();
    for (const auto& rule : rules().custom_rules) {
        if (!rule.enabled || !rule.pattern_valid) continue;
        if (!rule.languages.empty()) {
            bool matches = false;
            for (const auto& l : rule.languages) { if (l == language) { matches = true; break; } }
            if (!matches) continue;
        }
        try {
            if (std::regex_search(code, rule.compiled_pattern)) {
                std::string msg = rule.message.empty()
                    ? fmt::format("Custom rule '{}' violated", rule.name)
                    : rule.message;
                return enforce("custom_rules." + rule.id, rule.level,
                    formatError(rule.level, msg,
                        line > 0 ? fmt::format("line {}", line) : "",
                        "custom_rules[\"" + rule.id + "\"]",
                        rule.help, rule.bad_example, rule.good_example));
            }
        } catch (const std::regex_error& re) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", re.what());
        }
    }
    return "";
}

// --- Resource Limits ---
std::string GovernanceEngine::checkLoopIterations(size_t count) {
    clearTrace();
    int max = rules().limits.execution.loop_iterations;
    if (max > 0 && static_cast<int>(count) > max) {
        return enforce("limits.execution.loop_iterations", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Loop iteration count {} exceeds limit of {}", count, max),
                "", fmt::format("limits.execution.loop_iterations = {}", max),
                "Loop exceeded iteration limit — likely infinite or unbounded.\n"
                "Add a break condition, or process data in smaller chunks.",
                "while true { process() }  // unbounded",
                "for i in 0..len(items) { process(items[i]) }  // bounded by data"));
    }
    return "";
}

std::string GovernanceEngine::checkPolyglotBlockCount(size_t count) {
    clearTrace();
    int max = rules().limits.execution.polyglot_blocks;
    if (max > 0 && static_cast<int>(count) > max) {
        return enforce("limits.execution.polyglot_blocks", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Polyglot block count {} exceeds limit of {}", count, max),
                "", fmt::format("limits.execution.polyglot_blocks = {}", max),
                "Too many polyglot blocks executed — refactor to reduce block count.\n"
                "Combine related operations into fewer, larger blocks.", "", ""));
    }
    return "";
}

std::string GovernanceEngine::incrementAndCheckPolyglotBlockCount() {
    ++polyglot_block_count_;
    return checkPolyglotBlockCount(static_cast<size_t>(polyglot_block_count_));
}

std::string GovernanceEngine::checkStringLength(size_t length) {
    clearTrace();
    int max = rules().limits.data.string_length;
    if (max > 0 && static_cast<int>(length) > max) {
        return enforce("limits.data.string_length", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("String length {} exceeds limit of {}", length, max),
                "", fmt::format("limits.data.string_length = {}", max),
                "String exceeds maximum length — truncate or stream large data.\n"
                "Process large inputs in smaller segments.", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkNestingDepth(size_t depth) {
    clearTrace();
    int max = rules().limits.data.nesting_depth;
    if (max > 0 && static_cast<int>(depth) > max) {
        return enforce("limits.data.nesting_depth", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Nesting depth {} exceeds limit of {}", depth, max),
                "", fmt::format("limits.data.nesting_depth = {}", max),
                "Data structure nesting is too deep — flatten nested objects.\n"
                "Extract nested data into separate variables or use a flatter schema.", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkOutputSize(size_t size) {
    clearTrace();
    int max = rules().limits.data.output_size;
    if (max > 0 && static_cast<int>(size) > max) {
        return enforce("limits.data.output_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Output size {} exceeds limit of {}", size, max),
                "", fmt::format("limits.data.output_size = {}", max),
                "Polyglot block output exceeds size limit — return less data.\n"
                "Filter or summarize results instead of returning full datasets.", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkDictSize(size_t size) {
    clearTrace();
    int max = rules().limits.data.dict_size;
    if (max > 0 && static_cast<int>(size) > max) {
        return enforce("limits.data.dict_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Dictionary size {} exceeds limit of {}", size, max),
                "", fmt::format("limits.data.dict_size = {}", max),
                "Dictionary exceeds maximum entry count — use a smaller data structure.\n"
                "Split large dictionaries or process entries incrementally.", "", ""));
    }
    return "";
}

// --- Rate Limiting ---
bool GovernanceEngine::checkPolyglotRate() {
    polyglot_rate_.max_per_second = rules().limits.rate.max_polyglot_per_second;
    return polyglot_rate_.check();
}

bool GovernanceEngine::checkStdlibRate() {
    stdlib_rate_.max_per_second = rules().limits.rate.max_stdlib_calls_per_second;
    return stdlib_rate_.check();
}

bool GovernanceEngine::checkFileOpsRate() {
    file_ops_rate_.max_per_second = rules().limits.rate.max_file_ops_per_second;
    return file_ops_rate_.check();
}

// --- Per-Language Getters ---
int GovernanceEngine::getTimeoutForLanguage(const std::string& lang) const {
    auto it = rules().languages.per_language.find(lang);
    if (it != rules().languages.per_language.end() && it->second.timeout > 0)
        return it->second.timeout;
    if (rules().limits.timeout.per_block > 0) return rules().limits.timeout.per_block;
    return rules().timeout_seconds;
}

int GovernanceEngine::getMaxLinesForLanguage(const std::string& lang) const {
    // FIX 18: Normalize language alias before lookup
    std::string normalized = normalizeLanguage(lang);
    auto it = rules().languages.per_language.find(normalized);
    if (it != rules().languages.per_language.end() && it->second.max_lines > 0)
        return it->second.max_lines;
    return rules().limits.code.max_lines_per_block;
}

const LanguageConfig* GovernanceEngine::getLanguageConfig(const std::string& lang) const {
    // FIX 18: Normalize language alias before lookup
    std::string normalized = normalizeLanguage(lang);
    auto it = rules().languages.per_language.find(normalized);
    if (it != rules().languages.per_language.end()) return &it->second;
    return nullptr;
}

// --- Comprehensive Polyglot Block Check ---
std::string GovernanceEngine::checkVariableBinding(size_t binding_count, int line) {
    clearTrace();
    if (!rules().polyglot.variable_binding.require_explicit) return "";
    if (binding_count > 0) return "";  // Has bindings, OK

    std::string msg = "[governance] Polyglot block at line " + std::to_string(line) +
        " has no variable bindings.\n"
        "  Rule: polyglot.variable_binding.require_explicit = true\n"
        "  Fix: Add variable bindings: <<python[var1, var2] ... >>\n";

    return enforce("polyglot.variable_binding.require_explicit",
                   rules().polyglot.variable_binding.require_explicit_level, msg);
}

std::string GovernanceEngine::checkPolyglotBlock(
    const std::string& language, const std::string& code,
    const std::string& source_file, int line,
    size_t binding_count) {
    clearTrace();

    // Check variable binding requirement first
    std::string bind_err = checkVariableBinding(binding_count, line);
    if (!bind_err.empty()) return bind_err;

    // Delegate to existing comprehensive check
    return checkPolyglotBlock(language, code, source_file, line);
}

// C2 fix: expose preprocessing pipeline so naab_gov_check() C API can
// apply the same normalization as checkPolyglotBlock before dispatch
std::string GovernanceEngine::preprocessCode(const std::string& language, const std::string& code) {
    std::string lang = normalizeLanguage(language);
    std::string result = normalizeUnicode(code);
    result = normalizeWhitespace(result);
    result = stripStringLiterals(result);
    result = expandDangerousAliases(lang, result);
    return result;
}

std::string GovernanceEngine::checkPolyglotBlock(
    const std::string& language, const std::string& code,
    const std::string& source_file, int line) {

    // Set check context for report tracking (file + line propagate to enforce/recordPass)
    setCheckContext(source_file, line);

    // FIX 18: Normalize language aliases (bash→shell, js→javascript, etc.)
    std::string lang = normalizeLanguage(language);

    // Pre-process code — normalize Unicode homoglyphs and whitespace to prevent
    // evasion via confusable characters (Cyrillic е→e, fullwidth ｅ→e, zero-width
    // removals) and whitespace tricks (os.\nsystem, os . system, line continuations).
    // Normalization runs on BOTH raw code and stripped code paths.
    std::string normalized = normalizeUnicode(code);
    normalized = normalizeWhitespace(normalized);

    // FIX 16: Pre-process code — strip string literals for pattern matching
    // This prevents false positives from code/paths inside strings
    std::string stripped = stripStringLiterals(normalized);

    // Expand dangerous function aliases (e.g., s = os.system; s("rm") → os.system("rm"))
    // so downstream pattern-based checks catch indirect calls through variables
    stripped = expandDangerousAliases(lang, stripped);

    std::string err;

    // Language allowed? (uses normalized name)
    err = checkLanguageAllowed(lang, line);
    if (!err.empty()) return err;

    // Shell capability check
    if (lang == "shell") {
        err = checkShellAllowed();
        if (!err.empty()) return err;

        // naab-29 C-05: Enforce blocked_commands against shell code
        for (const auto& blocked : rules().capabilities.shell.blocked_commands) {
            if (stripped.find(blocked) != std::string::npos) {
                err = enforce("capabilities.shell.blocked_commands", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Blocked shell command detected: '{}'", blocked),
                        "",
                        "capabilities.shell.blocked_commands",
                        "This command is prohibited by governance policy",
                        fmt::format("<<shell\n{}\n>>", blocked),
                        "Use NAAb stdlib alternatives instead"));
                if (!err.empty()) return err;
            }
        }
    }

    // Code quality checks — secrets/PII use RAW code (secrets CAN be in strings)
    err = checkSecrets(code, line);
    if (!err.empty()) return err;
    err = checkPii(code, line);
    if (!err.empty()) return err;

    // Pattern-based checks use STRIPPED code to avoid string-content false positives
    // EVA-1 through EVA-4: Placeholders, temp code, simulation markers, and apologetic
    // language patterns match COMMENT text, so use `stripped` (comments preserved)
    // instead of `stripped_all` (comments removed) which made them 100% dead
    err = checkPlaceholders(stripped, line);
    if (!err.empty()) return err;
    err = checkHardcodedResults(stripped, line);
    if (!err.empty()) return err;
    err = checkDangerousCall(lang, stripped, line);
    if (!err.empty()) return err;
    // Some dangerous patterns require string content visible (e.g., preg_replace /e
    // modifier is inside the regex string). Check normalized (but not string-stripped) code.
    err = checkDangerousCall(lang, normalized, line);
    if (!err.empty()) return err;

    // New v3.0 checks — use stripped (strings removed, comments preserved)
    err = checkTemporaryCode(stripped, line);
    if (!err.empty()) return err;
    err = checkSimulationMarkers(stripped, line);
    if (!err.empty()) return err;
    err = checkMockData(code, line);  // Mock data: literals ARE in strings, keep raw
    if (!err.empty()) return err;
    err = checkApologeticLanguage(stripped, line);
    if (!err.empty()) return err;
    err = checkDeadCode(stripped, line);
    if (!err.empty()) return err;
    err = checkDebugArtifacts(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkUnsafeDeserialization(stripped, line);
    if (!err.empty()) return err;
    err = checkSqlInjection(code, line);  // SQL queries ARE in strings, keep raw
    if (!err.empty()) return err;
    err = checkPathTraversal(stripped, line);  // FIX 16: No more false positives from string paths
    if (!err.empty()) return err;
    err = checkHardcodedUrls(code, line);  // URLs ARE in strings
    if (!err.empty()) return err;
    err = checkHardcodedIps(code, line);   // IPs ARE in strings
    if (!err.empty()) return err;
    err = checkEncoding(stripped, line);
    if (!err.empty()) return err;
    err = checkComplexity(stripped, line);
    if (!err.empty()) return err;

    // LLM anti-drift checks
    err = checkOversimplification(stripped, line);
    if (!err.empty()) return err;
    err = checkIncompleteLogic(stripped, line);
    if (!err.empty()) return err;
    err = checkHallucinatedApis(lang, normalized, line);  // Has its own stripping
    if (!err.empty()) return err;
    err = checkSemanticIssues(lang, normalized, line);  // Has its own stripping
    if (!err.empty()) return err;

    // NOTE: Complexity floor intentionally NOT applied to polyglot blocks.
    // Polyglot blocks have their own quality checks (max_lines, banned_functions,
    // hallucinated APIs, security). The complexity floor is designed for NAAb function
    // bodies to prevent trivial compute_*/calculate_* stubs.

    // Security checks — use stripped code
    err = checkShellInjection(stripped, line);
    if (!err.empty()) return err;
    err = checkCodeInjection(lang, stripped, normalized, line);
    if (!err.empty()) return err;
    err = checkPrivilegeEscalation(stripped, line);
    if (!err.empty()) return err;
    // Privilege escalation commands (sudo, chmod +s) appear inside string args to
    // execution functions (os.system("sudo ..."), subprocess.run(["chmod", "+s", ...])).
    // Check normalized code to catch commands inside string literals.
    err = checkPrivilegeEscalation(normalized, line);
    if (!err.empty()) return err;
    err = checkDataExfiltration(stripped, line);
    if (!err.empty()) return err;
    err = checkResourceAbuse(stripped, line);
    if (!err.empty()) return err;
    err = checkInfoDisclosure(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkCryptoWeakness(stripped, normalized, line);
    if (!err.empty()) return err;
    err = checkVcsSecretExtraction(normalized, line);  // uses normalized raw code — secret targets in strings
    if (!err.empty()) return err;
    err = checkObfuscationSignals(lang, normalized, stripped, line);  // co-occurrence: normalized + stripped
    if (!err.empty()) return err;

    // Capability checks for polyglot blocks
    // NOTE: These checks use normalized (non-stripped) code because import/require
    // patterns match module names inside string literals (e.g., require("child_process")).
    // stripStringLiterals removes those names, making the patterns dead on stripped code.
    err = checkNetworkImports(lang, normalized, line);
    if (!err.empty()) return err;
    err = checkFilesystemImports(lang, normalized, line);
    if (!err.empty()) return err;

    // Per-language checks — checkImports matches require('mod')/import 'mod' patterns
    err = checkImports(lang, normalized, line);
    if (!err.empty()) return err;
    err = checkBannedFunctions(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkLanguageStyle(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkCodeSize(lang, code, line);  // Code size counts raw lines
    if (!err.empty()) return err;

    // Custom rules — use both raw and stripped
    err = checkCustomRules(lang, code, line);
    if (!err.empty()) return err;

    // Plugin rules (NAAb-based governance checks)
    err = checkPluginRules("polyglot_block", {
        {"code", interpreter::NaabVal::makeString(code)},
        {"language", interpreter::NaabVal::makeString(lang)},
        {"source_file", interpreter::NaabVal::makeString(current_check_file_)},
        {"line", interpreter::NaabVal::makeInt(line)},
    }, line);
    if (!err.empty()) return err;

    // Polyglot optimization (language choice suggestions)
    err = checkPolyglotOptimization(lang, code, line);
    if (!err.empty()) return err;

    // Telemetry: record successful polyglot governance pass
    if (rules().telemetry_output.enabled) {
        writeAgentTelemetry("POLYGLOT_EXEC", {
            {"language", lang},
            {"code_size", std::to_string(code.size())},
            {"source_file", source_file},
            {"line", std::to_string(line)},
            {"governance_epoch", std::to_string(getGovernanceEpoch())}
        });
    }

    return "";
}

// --- Schema Validation ---
size_t GovernanceEngine::levenshteinDistance(const std::string& s1, const std::string& s2) {
    size_t len1 = s1.size(), len2 = s2.size();
    std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));
    for (size_t i = 0; i <= len1; i++) d[i][0] = i;
    for (size_t j = 0; j <= len2; j++) d[0][j] = j;
    for (size_t i = 1; i <= len1; i++)
        for (size_t j = 1; j <= len2; j++) {
            size_t cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            d[i][j] = std::min({d[i-1][j]+1, d[i][j-1]+1, d[i-1][j-1]+cost});
        }
    return d[len1][len2];
}

std::string GovernanceEngine::suggestKey(const std::string& key,
                                          const std::vector<std::string>& valid_keys) {
    size_t best_dist = 999;
    std::string best_match;
    for (const auto& vk : valid_keys) {
        size_t dist = levenshteinDistance(key, vk);
        if (dist < best_dist && dist <= 3) { best_dist = dist; best_match = vk; }
    }
    return best_match;
}

std::vector<std::string> GovernanceEngine::validateSchema(const std::string& json_path) {
    std::vector<std::string> warnings;
    static const std::vector<std::string> VALID_TOP_KEYS = {
        "version", "mode", "extends", "description",
        "languages", "capabilities", "limits", "requirements",
        "restrictions", "code_quality", "custom_rules", "scopes",
        "output", "audit", "meta", "hooks", "polyglot", "polyglot_optimization",
        "contracts", "baselines", "project_context", "scanner",
        "governance_plugins", "governance",
        "taint_tracking", "quality_gate", "governance_baseline",
        "environments", "runtime_versions", "agent_roles", "agents", "telemetry", "scoring",
        "runtime", "security", "api", "integrity", "project_name", "scorers",
        "agent_review", "agent_dispatch", "sandbox_level", "approval",
        "behavioral_sequences", "context_drift",
        "trust", "prerequisites", "contradiction_detection",
        "exposure_tracking", "drift_detection",
        "circuit_breaker", "governance_health", "advisory_escalation",
        "pipeline_separation", "temporal_coupling",
        "subprocess_scrub_mode", "allowed_subprocess_vars",
        "blocked_subprocess_vars", "blocked_subprocess_prefixes",
        "codegen"
    };

    try {
        std::ifstream ifs(json_path);
        if (!ifs.is_open()) return warnings;
        auto j = nlohmann::json::parse(ifs);

        // Common abbreviations and misspellings that Levenshtein can't catch
        static const std::unordered_map<std::string, std::string> KNOWN_ALIASES = {
            {"cdd",                      "context_drift"},
            {"drift",                    "context_drift"},
            {"context_drift_detection",  "context_drift"},
            {"bsd",                      "behavioral_sequences"},
            {"behavioral_sequence",      "behavioral_sequences"},
            {"sequences",                "behavioral_sequences"},
            {"sandbox",                  "security"},
            {"sandbox_level",            "security"},
            {"agent_config",             "agents"},
            {"agent_roles_config",       "agents"},
        };

        for (auto& [key, val] : j.items()) {
            bool found = false;
            for (const auto& vk : VALID_TOP_KEYS) { if (key == vk) { found = true; break; } }
            if (!found) {
                // Check aliases first (Levenshtein misses semantic abbreviations)
                auto alias_it = KNOWN_ALIASES.find(key);
                std::string suggestion = (alias_it != KNOWN_ALIASES.end())
                    ? alias_it->second
                    : suggestKey(key, VALID_TOP_KEYS);
                if (!suggestion.empty()) {
                    warnings.push_back(fmt::format(
                        "[governance] Warning: Unknown key \"{}\" — did you mean \"{}\"?", key, suggestion));
                } else {
                    warnings.push_back(fmt::format(
                        "[governance] Warning: Unknown key \"{}\"", key));
                }
            }
        }
    } catch (const std::exception&) {}
    return warnings;
}

// --- Entropy Analysis ---
double GovernanceEngine::calculateEntropy(const std::string& str) {
    if (str.empty()) return 0.0;
    std::unordered_map<char, int> freq;
    for (char c : str) freq[c]++;
    double entropy = 0.0;
    double len = static_cast<double>(str.size());
    for (const auto& [ch, count] : freq) {
        double p = count / len;
        if (p > 0) entropy -= p * std::log2(p);
    }
    return entropy;
}

bool GovernanceEngine::looksLikeBase64(const std::string& str) {
    if (str.size() < 20) return false;
    try {
        std::regex re("^[A-Za-z0-9+/]+=*$");
        return std::regex_match(str, re);
    } catch (...) { return false; }
}

bool GovernanceEngine::looksLikeHex(const std::string& str) {
    if (str.size() < 20 || str.size() % 2 != 0) return false;
    try {
        std::regex re("^[0-9a-fA-F]+$");
        return std::regex_match(str, re);
    } catch (...) { return false; }
}

// --- Taint Tracking ---
void GovernanceEngine::markTainted(const std::string& var_name,
                                    const std::string& origin_func,
                                    const std::string& origin_arg,
                                    const std::string& file,
                                    int line) {
    if (!rules().taint_tracking.enabled) return;
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_.insert(var_name);
    if (rules().taint_tracking.lineage && !origin_func.empty()) {
        taint_lineage_[var_name] = {origin_func, origin_arg, file, line,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()};
    }
}

void GovernanceEngine::clearTaint(const std::string& var_name) {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_.erase(var_name);
    taint_lineage_.erase(var_name);
}

bool GovernanceEngine::isTainted(const std::string& var_name) const {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    return taint_set_.count(var_name) > 0;
}

std::optional<TaintMetadata> GovernanceEngine::getTaintLineage(const std::string& var_name) const {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    auto it = taint_lineage_.find(var_name);
    if (it != taint_lineage_.end()) return it->second;
    return std::nullopt;
}

// BUG-O: Save/restore taint state for module loading isolation
std::unordered_set<std::string> GovernanceEngine::saveTaintState() const {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    return taint_set_;
}

void GovernanceEngine::restoreTaintState(const std::unordered_set<std::string>& state) {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_ = state;
    // Clear lineage for vars no longer in taint set
    for (auto it = taint_lineage_.begin(); it != taint_lineage_.end(); ) {
        if (state.count(it->first) == 0) it = taint_lineage_.erase(it);
        else ++it;
    }
}

// V-GOV-004: counter state save/restore for async task inheritance.
// Each async task creates a fresh Interpreter with zeroed counters; restoring
// the parent's counters ensures limits like polyglot_blocks are enforced
// across the full execution (parent + all async tasks combined).
GovernanceEngine::CounterState GovernanceEngine::saveCounterState() const {
    CounterState s;
    s.polyglot_block_count = polyglot_block_count_;
    s.total_polyglot_lines = total_polyglot_lines_;
    s.advisory_count       = advisory_count_;
    s.advisory_suppressed  = advisory_suppressed_;
    return s;
}

void GovernanceEngine::restoreCounterState(const CounterState& s) {
    polyglot_block_count_ = s.polyglot_block_count;
    total_polyglot_lines_ = s.total_polyglot_lines;
    advisory_count_       = s.advisory_count;
    advisory_suppressed_  = s.advisory_suppressed;
}

// BUG-D: Track function return taint
bool GovernanceEngine::lastReturnWasTainted() const {
    return last_return_tainted_;
}

void GovernanceEngine::setLastReturnTainted(bool v) {
    last_return_tainted_ = v;
    if (!v) last_taint_source_.clear();
}

void GovernanceEngine::setLastReturnTainted(bool v, const std::string& source_func) {
    last_return_tainted_ = v;
    last_taint_source_ = v ? source_func : "";
}

bool GovernanceEngine::isTaintSource(const std::string& func_name) const {
    if (!rules().taint_tracking.enabled) return false;
    for (const auto& src : rules().taint_tracking.sources) {
        // FIX-DX-1: PREFIX match (not substring) to avoid false positives
        // e.g., "env.get" matches "env.get_var" but NOT "prevent.get"
        if (func_name.size() >= src.size() &&
            func_name.compare(0, src.size(), src) == 0) return true;
        // Also check after module dot: "json.env.get" → check "env.get" prefix
        auto dot = func_name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            // Check each dot-separated suffix for prefix match
            size_t pos = 0;
            while ((pos = func_name.find('.', pos)) != std::string::npos) {
                std::string suffix = func_name.substr(pos + 1);
                if (suffix.size() >= src.size() &&
                    suffix.compare(0, src.size(), src) == 0) return true;
                pos++;
            }
        }
    }
    return false;
}

bool GovernanceEngine::isSanitizer(const std::string& func_name) const {
    if (!rules().taint_tracking.enabled) return false;
    for (const auto& san : rules().taint_tracking.sanitizers) {
        // FIX-DX-1: PREFIX match (not substring) to avoid false positives
        // e.g., "validate_" matches "validate_input" but NOT "revalidate_input"
        // e.g., "int(" matches "int(x)" but NOT "print(x)" or "hint(x)"
        if (func_name.size() >= san.size() &&
            func_name.compare(0, san.size(), san) == 0) return true;
        // Also check after module dot: "utils.validate_input" → check "validate_" prefix
        size_t pos = 0;
        while ((pos = func_name.find('.', pos)) != std::string::npos) {
            std::string suffix = func_name.substr(pos + 1);
            if (suffix.size() >= san.size() &&
                suffix.compare(0, san.size(), san) == 0) return true;
            pos++;
        }
    }
    return false;
}

std::string GovernanceEngine::checkTaintedSink(const std::string& var_name,
                                                const std::string& sink_type,
                                                const std::string& file, int line) {
    if (!rules().taint_tracking.enabled) return "";
    clearTrace();
    {
        std::lock_guard<std::mutex> lock(taint_mutex_);
        if (taint_set_.count(var_name) == 0) return "";
        addTrace(fmt::format("variable '{}' is tainted", var_name));
        // Lineage: include origin info in trace if available (same lock scope)
        if (rules().taint_tracking.lineage) {
            auto lit = taint_lineage_.find(var_name);
            if (lit != taint_lineage_.end()) {
                const auto& meta = lit->second;
                addTrace(fmt::format("taint origin: {}('{}') at {}:{}",
                    meta.origin_function, meta.origin_argument,
                    meta.source_file, meta.source_line));
            }
        }
    }

    // FIX-DX-1: PREFIX match for sink types (not substring)
    // Auto-expand: file.append is equivalent to file.write for taint purposes
    std::string effective_sink = sink_type;
    if (sink_type == "file.append") effective_sink = "file.write";
    bool is_sink = false;
    for (const auto& s : rules().taint_tracking.sinks) {
        if (effective_sink.size() >= s.size() &&
            effective_sink.compare(0, s.size(), s) == 0) { is_sink = true; break; }
        // Also check the original sink_type for exact/prefix match
        if (sink_type.size() >= s.size() &&
            sink_type.compare(0, s.size(), s) == 0) { is_sink = true; break; }
        if (sink_type == s) { is_sink = true; break; }
    }
    if (!is_sink) {
        addTrace(fmt::format("'{}' not in configured sinks — allowed", sink_type));
        return "";
    }
    addTrace(fmt::format("tainted value reached sink '{}' without sanitization → BLOCKED", sink_type));

    // Log the decision
    logTaintDecision(var_name, "BLOCKED", sink_type, file, line);

    // Emit BSD event for behavioral sequence detection
    if (rules().behavioral_sequences.enabled) {
        emitEvent(RuntimeEventType::TAINT_VIOLATION,
            var_name + " → " + sink_type, file, line);
    }

    // Pass 2: Record taint flow for post-execution audit
    {
        std::lock_guard<std::mutex> lock(audit_data_mutex_);
        taint_flows_.push_back({var_name, "", sink_type, "BLOCKED", file, line});
    }

    // Use "variable 'x'" for real names, omit "variable" for synthetic labels like
    // "argument 0 of 'file.write()'" to produce cleaner error messages
    bool is_synthetic = var_name.find(' ') != std::string::npos;
    std::string msg = "Taint tracking violation: " +
                      std::string(is_synthetic ? "" : "variable ") + "'" + var_name +
                      "' contains untrusted data and reached sink '" + sink_type +
                      "' without sanitization";
    if (!file.empty()) msg += " at " + file + ":" + std::to_string(line);

    // Add sanitizer guidance (don't list specific sanitizers — adversarial LLMs
    // would use the list to create identity-function bypasses)
    msg += "\n\n  Help: Pass the value through a real sanitization function before\n"
           "  using it in a '" + sink_type + "' sink. Sanitizers must actually\n"
           "  validate or transform the data — identity functions are detected.\n";

    // Determine enforcement level from config
    EnforcementLevel taint_level = EnforcementLevel::HARD;
    if (rules().taint_tracking.level == "soft") taint_level = EnforcementLevel::SOFT;
    else if (rules().taint_tracking.level == "advisory") taint_level = EnforcementLevel::ADVISORY;
    else if (rules().taint_tracking.level == "detect") taint_level = EnforcementLevel::DETECT;

    // Record via enforce() so it appears in reports and audit trail
    std::string result = enforce("taint_tracking.sink_violation", taint_level, msg);
    return result;
}

// FIX-DX-8: Validate scope patterns against actual function names
void GovernanceEngine::validateScopePatterns(const std::vector<std::string>& function_names) {
    for (const auto& scope : rules().scopes) {
        if (scope.glob_pattern.empty()) continue;
        bool any_match = false;
        for (const auto& fn : function_names) {
            // Simple glob match: only supports * wildcard
            const std::string& pat = scope.glob_pattern;
            if (pat == "*") { any_match = true; break; }
            auto star = pat.find('*');
            if (star == std::string::npos) {
                // Exact match
                if (fn == pat) { any_match = true; break; }
            } else {
                // Prefix*suffix match
                std::string prefix = pat.substr(0, star);
                std::string suffix = pat.substr(star + 1);
                if (fn.size() >= prefix.size() + suffix.size() &&
                    fn.compare(0, prefix.size(), prefix) == 0 &&
                    (suffix.empty() || fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                    any_match = true;
                    break;
                }
            }
        }
        if (!any_match) {
            fmt::print(stderr, "[WARN] Governance scope (pattern '{}') "
                       "matched zero functions in this file.\n",
                       scope.glob_pattern);
        }
    }
}

// ============================================================================
// Pass 2: Determinism, Output Entropy, Error Dump Checks
// ============================================================================

std::string GovernanceEngine::checkDeterminism(const std::string& language,
                                                const std::string& code, int line) {
    clearTrace();
    static const std::vector<std::pair<std::string, std::string>> patterns = {
        // Random
        {R"(\brandom\b)", "random function"},
        {R"(\brand\s*\()", "rand() call"},
        {R"(Math\.random\(\))", "Math.random()"},
        {R"(\$RANDOM\b)", "shell $RANDOM"},
        {R"(\bshuffle\b)", "shuffle function"},
        // Time
        {R"(\btime\s*\()", "time() call"},
        {R"(datetime\.now)", "datetime.now()"},
        {R"(Date\.now\(\))", "Date.now()"},
        {R"(\bdate\b)", "date command"},
        {R"(clock\s*\()", "clock() call"},
        {R"(Time\.now)", "Time.now"},
        // UUID
        {R"(\buuid)", "UUID generation"},
        {R"(randomUUID)", "crypto.randomUUID()"},
        // Network
        {R"(requests\.get)", "HTTP request"},
        {R"(\bfetch\s*\()", "fetch() call"},
        {R"(\bcurl\b)", "curl command"},
        {R"(\bwget\b)", "wget command"},
        // Process
        {R"(\bgetpid\b)", "getpid()"},
        {R"(\$\$)", "shell PID"},
        // Temp files
        {R"(\bmktemp\b)", "mktemp"},
        {R"(\btmpfile\b)", "tmpfile()"},
        // Entropy sources
        {R"(/dev/u?random)", "/dev/urandom"},
    };

    for (const auto& [pat, desc] : patterns) {
        try {
            std::regex re(pat, std::regex::ECMAScript);
            if (std::regex_search(code, re)) {
                return "uses " + desc + " (non-deterministic)";
            }
        } catch (const std::regex_error& e) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", e.what());
        }
    }
    return "";
}

std::string GovernanceEngine::checkOutputEntropy(const std::string& output, int line) {
    clearTrace();
    // Check each line for high-entropy strings (possible leaked credentials)
    std::istringstream stream(output);
    std::string line_str;
    while (std::getline(stream, line_str)) {
        if (line_str.size() < 16) continue;  // Too short to be meaningful
        double entropy = calculateEntropy(line_str);
        if (entropy > 4.5) {
            return "high-entropy output (entropy=" +
                   fmt::format("{:.1f}", entropy) + ", possible credential leak)";
        }
    }
    return "";
}

std::string GovernanceEngine::checkErrorDumps(const std::string& output, int line) {
    clearTrace();
    static const std::vector<std::pair<std::string, std::string>> patterns = {
        {R"(Traceback \(most recent call last\))", "Python traceback"},
        {R"(at [\w\.]+\([\w\.]+:\d+\))", "Java/JS stack trace"},
        {R"(\bpanic:)", "Go/Rust panic"},
        {R"(Segmentation fault)", "segfault"},
        {R"(core dumped)", "core dump"},
        {R"(FATAL ERROR)", "fatal error"},
        {R"(undefined reference)", "linker error"},
        {R"(error\[E\d+\])", "Rust compiler error"},
    };

    for (const auto& [pat, desc] : patterns) {
        try {
            std::regex re(pat, std::regex::ECMAScript);
            if (std::regex_search(output, re)) {
                return "error dump detected: " + desc;
            }
        } catch (const std::regex_error& e) {
            fprintf(stderr, "[governance] Warning: regex pattern failed: %s\n", e.what());
        }
    }
    return "";
}

} // namespace governance
} // namespace naab
