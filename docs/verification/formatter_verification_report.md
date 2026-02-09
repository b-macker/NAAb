# Consolidated Report on `FORMATTER.md` Accuracy Verification

During the verification of `docs/reference/FORMATTER.md`, the `naab-lang fmt` tool exhibited multiple failures, indicating it is currently non-functional or severely buggy as described by the documentation.

**Summary of Failures:**

1.  **Basic Formatting (`naab-lang fmt <file.naab>`):**
    *   **Observed Behavior:** The formatter fails to apply basic formatting rules (e.g., spacing, indentation, line breaks) to unformatted NAAb code. It often reports parse errors during this process.
    *   **Impact:** The core functionality of automatically formatting code is broken.
2.  **Parse Errors During Formatting:**
    *   **Observed Behavior:** The formatter frequently encounters parse errors (e.g., `'let' statements must be inside a 'main {}' block or function`) even on syntactically valid (though unformatted) NAAb code, even when `let` statements are inside a function. This suggests a fundamental issue with the formatter's parser or its understanding of NAAb's top-level scoping rules.
    *   **Impact:** Prevents the formatter from working on many NAAb files.
3.  **Check Mode (`naab-lang fmt --check <file.naab>`):**
    *   **Observed Behavior:** Incorrectly reports formatted files as unformatted.
    *   **Impact:** The CI integration feature (`--check`) is unreliable.
4.  **Diff Mode (`naab-lang fmt --check --diff <file.naab>`):**
    *   **Observed Behavior:** Fails to correctly show differences between formatted and unformatted versions, often aborting due to parse errors.
    *   **Impact:** Developers cannot preview formatting changes.
5.  **Idempotence:**
    *   **Observed Behavior:** Fails because the initial formatting pass itself is unsuccessful.
    *   **Impact:** Code consistency cannot be guaranteed.
6.  **Custom Configuration (`--config`):**
    *   **Observed Behavior:** Custom settings (e.g., `indent_width`, `function_brace_style`) specified in a `.naabfmt.toml` file are not applied, or the formatter fails to parse the input with custom rules.
    *   **Impact:** Customization of code style is not possible.

**Conclusion:** The `naab-lang fmt` tool is **non-functional** and its documentation in `docs/reference/FORMATTER.md` is **highly inaccurate and misleading**.

**Action Needed:**

*   **Urgent investigation and bug fixing** of the `naab-lang fmt` tool. The parser issues are particularly critical.
*   Once fixed, comprehensively re-verify all aspects of `naab-lang fmt`.
*   Update `docs/reference/FORMATTER.md` to reflect the tool's current capabilities and correct usage.
