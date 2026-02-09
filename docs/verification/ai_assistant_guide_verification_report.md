# Consolidated Report on `AI_ASSISTANT_GUIDE.md` Accuracy Verification

During the verification of `docs/guides/AI_ASSISTANT_GUIDE.md` by extracting and running code examples, several inaccuracies and discrepancies were identified.

**Inaccuracies and Issues:**

1.  **Enum Syntax (Section: Advanced Type System -> Enums):**
    *   **Documentation Claim:** `let current = Status::ACTIVE`
    *   **Observed Behavior:** The correct syntax is `let current = Status.ACTIVE`. The `::` scope resolution operator is not used for enum access in NAAb.

2.  **String Conversion (Section: Advanced Type System -> Union Types):**
    *   **Documentation Claim:** `string.from_int(value)`
    *   **Observed Behavior:** The function `string.from_int` is not recognized (`Unknown function: from_int`). `json.stringify(value)` works as a viable alternative for string conversion.

3.  **Try/Catch Syntax (Section: Error Handling):**
    *   **Documentation Claim:** `catch error { ... }`
    *   **Observed Behavior:** This causes a parse error. The correct syntax is `catch (error) { ... }` (parentheses are required).

4.  **Integer Division Return Type (Section: Error Handling):**
    *   **Documentation Claim:** `fn divide(...) -> int { return a / b }`
    *   **Observed Behavior:** Division of integers (`10 / 2`) returns a `float` (`5.000000`), causing a type mismatch error if the function is typed to return `int`. The return type should be `float` or the result explicitly cast.

5.  **JavaScript Polyglot Syntax (Section: Polyglot Embedding):**
    *   **Observation:** A basic `console.log("...");` block triggered a `SyntaxError: expecting ')'` in the JS adapter. However, template literals (`` `...` ``) and array methods worked fine in other tests. This suggests a potential sensitivity in the JS parser or wrapper for simple string literals or function calls.

**Verified Working Features:**

*   **Basic Syntax:** Variables, types, print.
*   **Array/Dict Assignment:** Works as documented (new feature).
*   **Generics:** Struct and function definitions work.
*   **Null Safety:** Basic syntax accepted.
*   **Control Flow:** `if`, `while`, `for` (loops).
*   **Functions:** Definitions and calls work.
*   **Structs:** Definitions, `new` instantiation, and field access work.
*   **Polyglot:** Python, Bash, and C++ blocks generally work. Variable binding and return values (including complex types like Dicts and Lists) work correctly between NAAb and Python/JS.

**Recommendations:**

1.  Update the guide to use `.` for enums.
2.  Update the guide to use `catch (e)` syntax.
3.  Clarify return types for division or cast results.
4.  Investigate `string.from_int` status (implement it or update docs).
5.  Investigate JS adapter's handling of simple `console.log` statements.
