# Chapter 5: The Polyglot Block System

NAAb's most distinctive and powerful feature is its integrated polyglot block system. This system allows you to embed and execute code written in other programming languages directly within your NAAb program, treating these foreign code snippets as first-class citizens. This capability is central to NAAb's "Orchestrate in NAAb, Compute in the Best Language" philosophy.

## 5.1 The `<<lang` Syntax

The core of the polyglot system is the `<<lang` syntax. This construct tells the NAAb interpreter to switch to a specified foreign language, execute the code within the block, and then return control (and optionally, data) back to the NAAb program.

The basic syntax is:

```naab
<<language
    // Foreign code here
>>
```

Here, `language` is the identifier for the foreign language executor (e.g., `python`, `javascript`, `cpp`, `bash`). The code within the `<<` and `>>` delimiters is parsed and executed by the respective language's runtime.

### 5.1.1 Supported Languages

NAAb provides robust integration with a wide array of popular programming languages. The `<<lang` syntax works for the following:

*   **`python`**: For data science, scripting, and leveraging Python's vast ecosystem.
*   **`javascript`**: For lightweight scripting, JSON processing, and web technologies.
*   **`cpp`**: For high-performance, low-level computations, and critical algorithms.
*   **`bash` (or `sh`, `shell`)**: For system-level operations, shell scripting, and command execution.
*   **`rust`**: For performance-critical tasks with memory safety guarantees.
*   **`go`**: For concurrent programming and efficient system utilities.
*   **`ruby`**: For text processing, web scripting, and rapid prototyping.
*   **`csharp` (or `cs`)**: For enterprise applications and leveraging the .NET ecosystem.

Here are basic "Hello, World!" examples for some of these languages:

```naab
main {
    print("--- Polyglot Hello World Examples ---")

    <<python
    print("Hello from Python!")
    >>

    <<javascript
    console.log("Hello from JavaScript!");
    >>

    <<cpp
    #include <iostream>
    std::cout << "Hello from C++!" << std::endl;
    >>

    <<bash
    echo "Hello from Bash!"
    >>

    print("--- End Polyglot Hello World Examples ---")
}
```

## 5.2 Passing Variables into Blocks

One of the most powerful aspects of NAAb's polyglot system is the ability to seamlessly transfer data from your NAAb program into a foreign code block. This is achieved by listing the NAAb variables you want to bind to the foreign execution context within square brackets after the language identifier.

The syntax for passing variables is:

```naab
<<language[naab_var1, naab_var2, ...]
    // Foreign code can now access naab_var1, naab_var2
>>
```

NAAb handles the automatic serialization of your NAAb data types (int, float, string, bool, arrays, dictionaries, structs) into their equivalent representations in the target language (e.g., NAAb `int` to Python `int`, NAAb `array<string>` to JavaScript `Array`).

```naab
main {
    let username = "NAAbMaster"
    let user_id = 12345
    let permissions = ["read", "write", "execute"]
    let config = {"debug": true, "level": "info"}

    print("--- Passing Variables to Polyglot Blocks ---")

    // Python block accessing NAAb variables
    <<python[username, user_id, permissions, config]
    print(f"Python sees: User '{username}' (ID: {user_id})")
    print(f"Permissions: {permissions}")
    print(f"Config: {config}")
    if config["debug"]:
        print("Debug mode is ON in Python!")
    >>

    // JavaScript block accessing NAAb variables
    <<javascript[username, permissions]
    console.log(`JavaScript sees: User '${username}'`);
    console.log(`Permissions: ${permissions.join(', ')}`);
    >>

    // C++ block accessing NAAb variables
    // Note: Common headers like <iostream>, <string>, <vector> are automatically included.
    <<cpp[user_id]
    std::cout << "C++ sees User ID: " << user_id << std::endl;
    >>

    print("--- End Passing Variables ---")
}
```

## 5.3 Capturing Return Values from Blocks

Just as you can pass data into blocks, you can also capture results from foreign code execution and bring them back into your NAAb program. The value of the last expression evaluated in the foreign block is automatically converted back into a NAAb `Value` and can be assigned to a NAAb variable.

The syntax for capturing return values is:

```naab
let naab_result = <<language
    // Foreign code where the last expression is the return value
>>
```

NAAb handles the automatic deserialization of common data types from the foreign language's output back into NAAb's type system.

```naab
main {
    print("--- Capturing Return Values from Polyglot Blocks ---")

    // Python block returning an integer
    let python_sum: int = <<python
    a = 10
    b = 20
    a + b # Last expression is returned
    >>
    print("Python sum:", python_sum) // Output: Python sum: 30

    // JavaScript block returning a string
    let js_message: string = <<javascript
    const name = "World";
    `Hello, ${name} from JS!` // Last expression (template literal) is returned
    >>
    print("JavaScript message:", js_message) // Output: JavaScript message: Hello, World from JS!

    // Python returning a dictionary/object
    let python_data: dict<string, any> = <<python
    {"name": "PythonDict", "value": 100}
    >>
    print("Python dict:", python_data)
    print("Python dict name:", python_data["name"])

    // JavaScript returning an array
    let js_array: array<int> = <<javascript
    [10, 20, 30].map(x => x / 10)
    >>
    print("JavaScript array:", js_array) // Output: JavaScript array: [1, 2, 3]

    print("--- End Capturing Return Values ---")
}
```

By combining variable passing and return value capturing, NAAb allows you to build complex multi-language workflows where data flows seamlessly between your orchestrating NAAb code and specialized foreign language components.
