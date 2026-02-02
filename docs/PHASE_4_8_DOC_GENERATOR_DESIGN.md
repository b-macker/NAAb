# Phase 4.8: Documentation Generator (naab-doc) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** MEDIUM - Doc comment parsing and HTML generation
**Estimated Effort:** 2-3 weeks implementation
**Priority:** MEDIUM - Important for ecosystem

This document outlines the design for `naab-doc`, NAAb's documentation generator that extracts doc comments and generates HTML API documentation.

---

## Current Problem

**No Auto-Generated Documentation:**
- Must manually write and maintain documentation
- Code and docs get out of sync
- No standardized documentation format
- Difficult to generate API references
- No cross-references between types/functions

**Impact:** Poor documentation quality, outdated docs, difficult for users to learn APIs.

---

## Goals

### Primary Goals

1. **Doc Comment Syntax** - `///` for documentation
2. **Extract Comments** - Parse doc comments from source
3. **Generate HTML** - Create browsable documentation
4. **Cross-References** - Link to types/functions
5. **Search** - Full-text search in documentation

### Secondary Goals

6. **Markdown Support** - Write docs in markdown
7. **Code Examples** - Include code blocks
8. **Version Control** - Track docs across versions
9. **Custom Themes** - Configurable styling

---

## Doc Comment Syntax

### Function Documentation

```naab
/// Adds two integers together.
///
/// This function performs addition on two integer values
/// and returns the sum.
///
/// # Arguments
/// * `a` - The first number
/// * `b` - The second number
///
/// # Returns
/// The sum of `a` and `b`
///
/// # Example
/// ```naab
/// let result = add(2, 3)
/// assert_eq(result, 5)
/// ```
function add(a: int, b: int) -> int {
    return a + b
}
```

### Struct Documentation

```naab
/// Represents a point in 2D space.
///
/// A Point contains x and y coordinates,
/// which can be positive, negative, or zero.
struct Point {
    /// X coordinate
    x: int

    /// Y coordinate
    y: int
}
```

### Enum Documentation

```naab
/// HTTP status codes.
///
/// Standard HTTP response status codes
/// as defined in RFC 2616.
enum HttpStatus {
    /// Success - Request completed successfully
    Ok = 200

    /// Not Found - Resource doesn't exist
    NotFound = 404

    /// Internal Server Error
    InternalServerError = 500
}
```

---

## Doc Comment Format

### Markdown Support

**Headers:**
```naab
/// # Main Header
/// ## Sub Header
/// ### Sub-sub Header
```

**Lists:**
```naab
/// * Item 1
/// * Item 2
///   * Nested item
```

**Code Blocks:**
```naab
/// Example usage:
/// ```naab
/// let x = 42
/// print(x)
/// ```
```

**Links:**
```naab
/// See [HttpClient](#HttpClient) for more details.
/// External link: [RFC 2616](https://tools.ietf.org/html/rfc2616)
```

**Emphasis:**
```naab
/// This is *italic* and **bold** text.
/// This is `code`.
```

---

## Doc Comment Parsing

### DocComment AST Node

```cpp
struct DocComment {
    std::string summary;      // First paragraph
    std::string description;  // Full description
    std::vector<Param> params;
    std::string returns;
    std::vector<Example> examples;
    std::vector<SeeAlso> see_also;
};

struct Param {
    std::string name;
    std::string description;
};

struct Example {
    std::string code;
    std::string description;
};

struct SeeAlso {
    std::string target;
    std::string description;
};
```

### Parser Integration

```cpp
class Parser {
    std::unique_ptr<FunctionDecl> parseFunctionDecl() {
        // Extract doc comment (if any)
        DocComment doc_comment = extractDocComment();

        // Parse function
        expect(TokenType::FUNCTION, "Expected 'function'");
        // ... rest of function parsing

        auto func = std::make_unique<FunctionDecl>(...);
        func->setDocComment(doc_comment);
        return func;
    }

    DocComment extractDocComment() {
        DocComment doc;

        // Look for /// comments before current token
        int line = current().line - 1;
        std::vector<std::string> lines;

        // Collect all /// lines
        while (line >= 0 && isDocCommentLine(line)) {
            lines.insert(lines.begin(), getLine(line));
            line--;
        }

        if (lines.empty()) {
            return doc;
        }

        // Parse doc comment
        doc = parseDocCommentLines(lines);
        return doc;
    }

    DocComment parseDocCommentLines(const std::vector<std::string>& lines) {
        DocComment doc;
        std::ostringstream current_section;
        std::string section_type;

        for (const auto& line : lines) {
            std::string content = line.substr(3);  // Remove "///"

            if (content.starts_with(" # Arguments")) {
                section_type = "arguments";
            } else if (content.starts_with(" # Returns")) {
                section_type = "returns";
            } else if (content.starts_with(" # Example")) {
                section_type = "example";
            } else if (section_type == "arguments") {
                // Parse: * `param_name` - Description
                if (content.starts_with(" * `")) {
                    Param param = parseParamDoc(content);
                    doc.params.push_back(param);
                }
            } else if (section_type == "returns") {
                doc.returns += content + "\n";
            } else if (section_type == "example") {
                // Collect example code
                current_section << content << "\n";
            } else {
                // Description
                if (doc.summary.empty()) {
                    doc.summary = content;
                } else {
                    doc.description += content + "\n";
                }
            }
        }

        return doc;
    }
};
```

---

## HTML Generation

### Output Structure

```
docs/
├── index.html              # Main page
├── functions/
│   ├── add.html
│   └── subtract.html
├── structs/
│   ├── Point.html
│   └── Person.html
├── enums/
│   └── HttpStatus.html
├── search.js               # Search index
└── style.css               # Styling
```

### HTML Templates

**Function Page:**

```html
<!DOCTYPE html>
<html>
<head>
    <title>add - NAAb Documentation</title>
    <link rel="stylesheet" href="../style.css">
</head>
<body>
    <nav>
        <a href="../index.html">Home</a>
        <input type="text" id="search" placeholder="Search...">
    </nav>

    <main>
        <h1>Function: <code>add</code></h1>

        <div class="signature">
            <code>function add(a: int, b: int) -> int</code>
        </div>

        <div class="summary">
            Adds two integers together.
        </div>

        <div class="description">
            This function performs addition on two integer values
            and returns the sum.
        </div>

        <h2>Arguments</h2>
        <dl>
            <dt><code>a</code></dt>
            <dd>The first number</dd>

            <dt><code>b</code></dt>
            <dd>The second number</dd>
        </dl>

        <h2>Returns</h2>
        <p>The sum of <code>a</code> and <code>b</code></p>

        <h2>Example</h2>
        <pre><code class="language-naab">
let result = add(2, 3)
assert_eq(result, 5)
        </code></pre>
    </main>
</body>
</html>
```

---

## Documentation Generator

### DocGenerator Class

```cpp
class DocGenerator {
public:
    DocGenerator(const std::string& source_dir, const std::string& output_dir);

    void generate();

private:
    std::string source_dir_;
    std::string output_dir_;

    std::vector<DocItem> items_;

    void discoverFiles();
    void parseFiles();
    void generateHTML();
    void generateIndex();
    void generateSearchIndex();

    std::string renderFunction(const FunctionDocItem& func);
    std::string renderStruct(const StructDocItem& struct_);
    std::string renderEnum(const EnumDocItem& enum_);
};

struct DocItem {
    std::string name;
    std::string file;
    DocComment comment;
};

struct FunctionDocItem : DocItem {
    std::vector<Param> params;
    Type return_type;
};

struct StructDocItem : DocItem {
    std::vector<Field> fields;
};

struct EnumDocItem : DocItem {
    std::vector<Variant> variants;
};
```

### Generation Process

```cpp
void DocGenerator::generate() {
    std::cout << "Generating documentation...\n";

    // 1. Discover all source files
    discoverFiles();

    // 2. Parse files and extract doc comments
    parseFiles();

    // 3. Generate HTML pages
    generateHTML();

    // 4. Generate index page
    generateIndex();

    // 5. Generate search index
    generateSearchIndex();

    std::cout << "Documentation generated in " << output_dir_ << "\n";
}

void DocGenerator::parseFiles() {
    for (const auto& file : source_files_) {
        Parser parser(file);
        auto program = parser.parse();

        // Extract doc items
        for (const auto& decl : program->getDeclarations()) {
            if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
                if (func->hasDocComment()) {
                    FunctionDocItem item;
                    item.name = func->getName();
                    item.file = file;
                    item.comment = func->getDocComment();
                    item.params = func->getParams();
                    item.return_type = func->getReturnType();
                    items_.push_back(item);
                }
            }
            // ... similar for structs, enums, etc.
        }
    }
}

void DocGenerator::generateHTML() {
    for (const auto& item : items_) {
        std::string html;

        if (auto* func = dynamic_cast<FunctionDocItem*>(&item)) {
            html = renderFunction(*func);
        } else if (auto* struct_ = dynamic_cast<StructDocItem*>(&item)) {
            html = renderStruct(*struct_);
        }

        // Write to file
        std::string output_path = getOutputPath(item);
        std::ofstream file(output_path);
        file << html;
    }
}
```

---

## Cross-References

### Automatic Linking

**Doc comment:**
```naab
/// Uses [Point](#Point) to represent coordinates.
/// Returns [HttpResponse](#HttpResponse) on success.
```

**Generated HTML:**
```html
Uses <a href="../structs/Point.html">Point</a> to represent coordinates.
Returns <a href="../structs/HttpResponse.html">HttpResponse</a> on success.
```

**Implementation:**

```cpp
std::string DocGenerator::renderMarkdown(const std::string& markdown) {
    std::string html = markdown;

    // Replace [Name](#Name) with links
    std::regex link_pattern(R"(\[([^\]]+)\]\(#([^\)]+)\))");
    html = std::regex_replace(html, link_pattern, [this](const std::smatch& match) {
        std::string text = match[1];
        std::string target = match[2];

        // Find target in items
        std::string url = findItemUrl(target);
        if (!url.empty()) {
            return "<a href=\"" + url + "\">" + text + "</a>";
        }

        return text;  // No link if not found
    });

    return html;
}
```

---

## Search Functionality

### Search Index

**Generate JSON index:**

```json
{
  "items": [
    {
      "name": "add",
      "type": "function",
      "summary": "Adds two integers together",
      "url": "functions/add.html"
    },
    {
      "name": "Point",
      "type": "struct",
      "summary": "Represents a point in 2D space",
      "url": "structs/Point.html"
    }
  ]
}
```

**JavaScript Search:**

```javascript
// search.js
let searchIndex = [];

fetch('search.json')
    .then(r => r.json())
    .then(data => {
        searchIndex = data.items;
    });

function search(query) {
    query = query.toLowerCase();
    return searchIndex.filter(item => {
        return item.name.toLowerCase().includes(query) ||
               item.summary.toLowerCase().includes(query);
    });
}

document.getElementById('search').addEventListener('input', (e) => {
    const results = search(e.target.value);
    displayResults(results);
});
```

---

## CLI Interface

### Commands

```bash
# Generate docs from source
naab-doc src/

# Specify output directory
naab-doc src/ --output docs/

# Custom title
naab-doc src/ --title "My Project API"

# Include private items
naab-doc src/ --private

# Custom theme
naab-doc src/ --theme dark

# Open in browser after generating
naab-doc src/ --open
```

### Example Output

```
$ naab-doc src/
Generating documentation...
Discovered 15 source files
Extracted 42 documented items:
  - 25 functions
  - 10 structs
  - 7 enums
Generated HTML pages
Created search index
Documentation generated in docs/

View at: file:///.../docs/index.html
```

---

## Themes

### Default Theme (Light)

**style.css:**
```css
:root {
    --bg-color: #ffffff;
    --text-color: #333333;
    --link-color: #0066cc;
    --code-bg: #f5f5f5;
    --border-color: #dddddd;
}

body {
    font-family: 'Segoe UI', sans-serif;
    line-height: 1.6;
    color: var(--text-color);
    background: var(--bg-color);
    max-width: 1200px;
    margin: 0 auto;
    padding: 20px;
}

code {
    background: var(--code-bg);
    padding: 2px 6px;
    border-radius: 3px;
    font-family: 'Consolas', monospace;
}

pre code {
    display: block;
    padding: 15px;
    overflow-x: auto;
}
```

### Dark Theme

Override CSS variables:
```css
:root {
    --bg-color: #1e1e1e;
    --text-color: #d4d4d4;
    --link-color: #4fc3f7;
    --code-bg: #2d2d2d;
    --border-color: #444444;
}
```

---

## Integration with Build System

### Auto-generate on build

**naab.build.json:**

```json
{
  "scripts": {
    "docs": "naab-doc src/ --output docs/",
    "build": "naab-build && naab-doc src/"
  }
}
```

---

## Implementation Plan

### Week 1: Doc Comment Parsing (5 days)

- [ ] Implement DocComment AST node
- [ ] Update parser to extract doc comments
- [ ] Parse markdown in comments
- [ ] Test: Doc comments extracted correctly

### Week 2: HTML Generation (5 days)

- [ ] Implement DocGenerator
- [ ] HTML templates for function/struct/enum
- [ ] Cross-reference linking
- [ ] Test: HTML pages generated

### Week 3: Search & Polish (5 days)

- [ ] Generate search index
- [ ] JavaScript search functionality
- [ ] CLI interface
- [ ] Themes and styling
- [ ] Documentation

**Total: 3 weeks**

---

## Success Metrics

### Phase 4.8 Complete When:

- [x] Doc comment syntax working
- [x] Parser extracts doc comments
- [x] HTML documentation generated
- [x] Cross-references working
- [x] Search functionality working
- [x] Multiple themes supported
- [x] CLI interface complete
- [x] Documentation complete

---

## Conclusion

**Phase 4.8 Status: DESIGN COMPLETE**

A documentation generator will:
- Auto-generate API documentation
- Keep docs in sync with code
- Improve discoverability
- Professional documentation website

**Implementation Effort:** 3 weeks

**Priority:** MEDIUM (important for ecosystem)

**Dependencies:** Parser with doc comment support

Once implemented, NAAb will have documentation generation like Rustdoc, JSDoc, or Javadoc.
