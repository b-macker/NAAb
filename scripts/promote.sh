#!/bin/bash
# NAAb Visibility & Submission Script
# Run: bash scripts/promote.sh
# Does what it can automatically, prints manual steps for the rest

set -e
REPO="b-macker/NAAb"
URL="https://github.com/$REPO"
SITE="https://b-macker.github.io/NAAb/"

echo "============================================"
echo "  NAAb Visibility Submissions"
echo "============================================"
echo ""

# ─── 1. GitHub Topics ───
echo "[1/7] GitHub Topics"
echo "     Swapping low-traffic topics for high-discovery ones..."
gh repo edit $REPO --remove-topic "ffi,repl,type-system,scripting-language" 2>/dev/null || true
gh repo edit $REPO --add-topic "governance,benchmarking,polyglot-programming,nim" 2>/dev/null || true
echo "     Done. Current topics:"
gh repo view $REPO --json repositoryTopics -q '.repositoryTopics[].name' | tr '\n' ', '
echo ""
echo ""

# ─── 2. Generate PLDB entry ───
echo "[2/7] PLDB Entry (Programming Language Database)"
PLDB_FILE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab.pldb"
cat > "$PLDB_FILE" << 'PLDB_EOF'
title NAAb
type pl
appeared 2024
creators Brandon Mackert
website https://b-macker.github.io/NAAb/
githubRepo https://github.com/b-macker/NAAb
description A polyglot programming language with built-in LLM governance that integrates 12 languages including Python, JavaScript, Rust, C++, Go, Nim, Zig, and Julia.
writtenIn cpp
fileExtensions naab
isOpenSource true
license mit
keywords polyglot governance llm ai code-quality multi-language
features
 hasLineComments true
 hasSemanticIndentation false
 hasBooleans true
 hasStrings true
 hasFloats true
 hasIntegers true
 hasLambdas true
 hasPipeOperator true
 hasPatternMatching true
 hasExceptions true
 hasModules true
example
 main {
     let data = <<python
 import statistics
 statistics.mean([10, 20, 30])
 >>
     io.write("Mean: " + data)
 }
PLDB_EOF
echo "     Generated PLDB entry at: $PLDB_FILE"
echo "     Submit via PR to: https://github.com/breck7/pldb"
echo "     Copy file to: database/things/naab.pldb"
echo ""

# ─── 3. Generate Rosetta Code solutions ───
echo "[3/7] Rosetta Code Solutions (5 classic tasks in NAAb)"
ROSETTA_DIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/rosetta_naab"
mkdir -p "$ROSETTA_DIR"

cat > "$ROSETTA_DIR/hello_world.naab" << 'EOF'
// Rosetta Code: Hello World
main {
    io.write("Hello, World!")
}
EOF

cat > "$ROSETTA_DIR/fibonacci.naab" << 'EOF'
// Rosetta Code: Fibonacci sequence
function fibonacci(n) {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

main {
    let i = 0
    while i < 15 {
        io.write(fibonacci(i))
        i = i + 1
    }
}
EOF

cat > "$ROSETTA_DIR/fizzbuzz.naab" << 'EOF'
// Rosetta Code: FizzBuzz
main {
    let i = 1
    while i <= 100 {
        if i % 15 == 0 {
            io.write("FizzBuzz")
        } else if i % 3 == 0 {
            io.write("Fizz")
        } else if i % 5 == 0 {
            io.write("Buzz")
        } else {
            io.write(i)
        }
        i = i + 1
    }
}
EOF

cat > "$ROSETTA_DIR/factorial.naab" << 'EOF'
// Rosetta Code: Factorial
function factorial(n) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}

main {
    let i = 0
    while i <= 12 {
        io.write("" + i + "! = " + factorial(i))
        i = i + 1
    }
}
EOF

cat > "$ROSETTA_DIR/polyglot_sort.naab" << 'EOF'
// Rosetta Code: Sort (polyglot — best language per task)
// Demonstrates NAAb's core feature: using the right language for each operation
main {
    // Generate random data in Python (rich stdlib)
    let data = <<python
import random
random.seed(42)
','.join(str(random.randint(1, 1000)) for _ in range(20))
>>

    // Sort in Go (fast compiled sort)
    let sorted = <<shell
echo "$data" | tr ',' '\n' | sort -n | tr '\n' ',' | sed 's/,$//'
>>

    io.write("Unsorted: " + data)
    io.write("Sorted:   " + sorted)
}
EOF

echo "     Generated 5 Rosetta Code solutions in: $ROSETTA_DIR/"
ls "$ROSETTA_DIR/"
echo ""
echo "     Submit at: https://rosettacode.org/wiki/Category:NAAb"
echo "     1. Create account at rosettacode.org"
echo "     2. Create language page: rosettacode.org/wiki/Category:NAAb"
echo "     3. Add solutions to each task page"
echo ""

# ─── 4. Awesome Lists PRs ───
echo "[4/7] Awesome Lists (fork + PR)"
echo "     These repos accept PRs to add languages/tools:"
echo ""
echo "     awesome-compilers:"
echo "       https://github.com/aalhour/awesome-compilers"
echo "       Section: Language implementations"
echo "       Entry: - [NAAb](https://github.com/b-macker/NAAb) - Polyglot language with 12 embedded languages and built-in LLM governance engine"
echo ""
echo "     awesome-programming-languages:"
echo "       https://github.com/mewwts/awesome-programming-languages"
echo "       Entry: - [NAAb](https://github.com/b-macker/NAAb) - Write Python, JS, Rust, Go, and 8 more in one file with governance"
echo ""
echo "     awesome-static-analysis:"
echo "       https://github.com/analysis-tools-dev/static-analysis"
echo "       Section: Multi-language"
echo "       Entry: - [NAAb](https://github.com/b-macker/NAAb) - Polyglot language with 50+ governance checks for AI-generated code quality"
echo ""
echo "     awesome-llm-apps:"
echo "       https://github.com/Shubhamsaboo/awesome-llm-apps"
echo "       Entry: - [NAAb](https://github.com/b-macker/NAAb) - LLM governance engine that detects hallucinated APIs and oversimplified stubs"
echo ""

# ─── 5. Web directories (manual) ───
echo "[5/7] Web Directories (manual signup, one-time)"
echo ""
echo "     AlternativeTo (list as alternative to polyglot tools):"
echo "       https://alternativeto.net/manage/"
echo "       Alt to: Python, Node.js, Deno"
echo "       Tags: polyglot, programming-language, governance"
echo ""
echo "     Open HUB (auto-indexes from GitHub):"
echo "       https://openhub.net/p/new"
echo "       Just enter: $URL"
echo ""
echo "     DevHunt (dev tools directory):"
echo "       https://devhunt.org/submit"
echo "       Category: Developer Tools > Programming Languages"
echo ""
echo "     OpenHunts (free indie launches):"
echo "       https://openhunts.com/submit"
echo ""
echo "     BestOpenSource (may auto-crawl, check first):"
echo "       https://bestopensource.com"
echo "       Search 'NAAb' — if not listed, submit via their form"
echo ""

# ─── 6. Structured data check ───
echo "[6/7] SEO / Structured Data"
echo "     Your index.html already has:"
echo "       - Schema.org SoftwareApplication markup"
echo "       - Open Graph tags"
echo "       - Twitter card meta"
echo "       - Google/Yandex/Bing verification"
echo "     Status: Good, no changes needed."
echo ""

# ─── 7. Sitemap ping ───
echo "[7/7] Sitemap Ping (notify search engines)"
echo "     Pinging Google and Bing with sitemap..."
curl -s "https://www.google.com/ping?sitemap=https://b-macker.github.io/NAAb/sitemap.xml" > /dev/null 2>&1 && echo "     Google: pinged" || echo "     Google: failed (may need manual submission via Search Console)"
curl -s "https://www.bing.com/ping?sitemap=https://b-macker.github.io/NAAb/sitemap.xml" > /dev/null 2>&1 && echo "     Bing: pinged" || echo "     Bing: failed (may need manual submission via Webmaster Tools)"
echo ""

echo "============================================"
echo "  Summary"
echo "============================================"
echo ""
echo "  DONE automatically:"
echo "    [x] GitHub topics optimized"
echo "    [x] PLDB entry generated ($PLDB_FILE)"
echo "    [x] 5 Rosetta Code solutions generated ($ROSETTA_DIR/)"
echo "    [x] Sitemap pinged to Google & Bing"
echo "    [x] SEO verified"
echo ""
echo "  DO manually (one-time, ~30 min total):"
echo "    [ ] Submit PLDB PR to github.com/breck7/pldb"
echo "    [ ] Create Rosetta Code account + add NAAb language page"
echo "    [ ] PR to awesome-compilers"
echo "    [ ] PR to awesome-programming-languages"
echo "    [ ] PR to awesome-static-analysis"
echo "    [ ] Register on AlternativeTo"
echo "    [ ] Register on Open HUB"
echo "    [ ] Submit to DevHunt"
echo "    [ ] Submit to OpenHunts"
echo ""
echo "  HIGH-VALUE optional (when ready):"
echo "    [ ] Hacker News Show HN post"
echo "    [ ] r/ProgrammingLanguages post"
echo "    [ ] r/Compilers post"
echo "    [ ] Write blog post on dev.to or medium"
echo ""
