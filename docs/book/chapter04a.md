# Chapter 4.5: Polyglot Async Execution

One of NAAb's most powerful features is the ability to execute code from multiple programming languages concurrently in separate threads. This chapter covers how to use polyglot async execution to leverage the strengths of Python, JavaScript, C++, and other languages for parallel processing.

## 4.5.1 Introduction to Polyglot Async

**What is Polyglot Async?**

Polyglot async allows you to execute code blocks from different programming languages in parallel threads without blocking the main NAAb interpreter. Each polyglot block (`<<language code>>`) runs in its own thread with proper timeout handling and thread-safe execution.

**Supported Languages:**
- **Python** - ML/AI, data processing, web scraping
- **JavaScript** - Data transformation, JSON processing
- **C++** - High-performance computing
- **Rust** - Systems programming
- **C#** - .NET integration
- **Shell** - System commands, file operations
- **Generic Subprocess** - Any command-line tool

**Key Features:**
- ✅ True parallelism (runs in separate OS threads)
- ✅ Thread-safe execution with proper synchronization
- ✅ Timeout support (prevents infinite loops)
- ✅ Type conversion between languages
- ✅ Security hardened (FFI validation, input caps)

---

## 4.5.2 Basic Polyglot Execution

### Simple Example

```naab
use io

main {
    # Execute Python code and get the result
    let result = <<python
    42 + 8
    >>

    io.write("Python result: ", result, "\n")  # Output: Python result: 50
}
```

### Multiple Languages

```naab
use io
use json

main {
    # Python for data processing
    let python_result = <<python
    import math
    math.sqrt(144)
    >>

    # JavaScript for JSON manipulation
    let js_result = <<javascript
    const data = {name: "NAAb", version: 1.0};
    JSON.stringify(data)
    >>

    # Shell for system commands
    let shell_result = <<bash
    echo "Hello from shell"
    >>

    io.write("Python: ", python_result, "\n")       # 12.0
    io.write("JavaScript: ", js_result, "\n")       # {"name":"NAAb","version":1.0}
    io.write("Shell: ", shell_result, "\n")         # Hello from shell
}
```

---

## 4.5.3 Parallel Execution

**The Power of Async:** Multiple polyglot blocks execute in parallel!

```naab
use io
use time

main {
    io.write("Starting parallel execution...\n")

    let start = time.now()

    # These THREE operations run in PARALLEL threads:
    let result1 = <<python
    import time
    time.sleep(2)  # Simulates slow operation
    return "Python done"
    >>

    let result2 = <<javascript
    // Simulates data processing
    const arr = Array(1000000).fill(0).map((_, i) => i * 2);
    "JavaScript done"
    >>

    let result3 = <<bash
    sleep 1 && echo "Shell done"
    >>

    let elapsed = time.now() - start

    io.write("All done in ", elapsed, " seconds\n")
    # Output: All done in ~2 seconds (not 3+!)
    # Proof they ran in parallel!
}
```

---

## 4.5.4 Real-World Use Cases

### Use Case 1: Parallel API Calls

```naab
use io
use json

main {
    # Fetch data from multiple APIs in parallel
    let users = <<python
    import requests
    requests.get('https://api.example.com/users').json()
    >>

    let posts = <<python
    import requests
    requests.get('https://api.example.com/posts').json()
    >>

    let comments = <<python
    import requests
    requests.get('https://api.example.com/comments').json()
    >>

    # All three API calls happened in parallel!
    io.write("Fetched: ", json.stringify(users), "\n")
}
```

### Use Case 2: ML Model Inference

```naab
use io

main {
    # Load and run ML model in Python (doesn't block NAAb)
    let prediction = <<python
    import torch
    import torchvision

    model = torchvision.models.resnet50(pretrained=True)
    model.eval()

    # Process image...
    prediction = model(image_tensor)
    prediction.tolist()
    >>

    io.write("Prediction: ", prediction, "\n")
}
```

### Use Case 3: Data Processing Pipeline

```naab
use io

main {
    # Step 1: Extract data (Python)
    let raw_data = <<python
    import pandas as pd
    df = pd.read_csv('data.csv')
    df.to_json()
    >>

    # Step 2: Transform data (JavaScript)
    let transformed = <<javascript
    const data = JSON.parse(raw_data);
    // Apply transformations...
    JSON.stringify(processed)
    >>

    # Step 3: Analyze (Python with NumPy)
    let analysis = <<python
    import numpy as np
    import json

    data = json.loads(transformed)
    stats = {
        'mean': np.mean(data['values']),
        'std': np.std(data['values'])
    }
    json.dumps(stats)
    >>

    io.write("Analysis: ", analysis, "\n")
}
```

### Use Case 4: Parallel File Processing

```naab
use io

main {
    # Process multiple files in parallel
    let file1_data = <<python
    with open('file1.txt', 'r') as f:
        return f.read().upper()
    >>

    let file2_data = <<python
    with open('file2.txt', 'r') as f:
        return f.read().lower()
    >>

    let file3_data = <<python
    with open('file3.txt', 'r') as f:
        return len(f.read())
    >>

    # All files read in parallel!
}
```

---

## 4.5.5 Variable Binding

You can pass NAAb variables into polyglot blocks:

```naab
use io

main {
    let multiplier = 10
    let values = [1, 2, 3, 4, 5]

    let result = <<python[multiplier, values]
    # multiplier and values are available in Python
    [x * multiplier for x in values]
    >>

    io.write("Result: ", result, "\n")  # [10, 20, 30, 40, 50]
}
```

**Syntax:** `<<language[var1, var2, ...] code >>`

---

## 4.5.6 Shell Results

Shell commands return the standard output as a string. The output is automatically trimmed of trailing whitespace.

```naab
use io

main {
    let result = <<bash
    ls -la /tmp | head -5
    >>

    # Result is a string containing stdout
    io.write("Output: ", result, "\n")
}
```

To detect errors in shell commands, use try/catch — a non-zero exit code raises an exception:

```naab
use io

main {
    try {
        let result = <<bash
        ls /nonexistent_directory
        >>
    } catch (e) {
        io.write("Command failed: ", e, "\n")
    }
}
```

**Shell Operators Supported:**
- `&&` - AND operator
- `||` - OR operator
- `|` - Pipe operator
- `;` - Sequential execution
- `>`, `<` - Redirection

```naab
let result = <<bash
echo "test" > /tmp/test.txt && cat /tmp/test.txt
>>
# Both commands execute in sequence
```

---

## 4.5.7 Type Conversion

NAAb automatically converts types between languages:

| NAAb Type | Python | JavaScript | Return to NAAb |
|-----------|--------|------------|----------------|
| `int` | `int` | `number` | `int` |
| `float` | `float` | `number` | `float` |
| `string` | `str` | `string` | `string` |
| `bool` | `bool` | `boolean` | `bool` |
| `list<T>` | `list` | `Array` | `list<T>` |
| `dict<K,V>` | `dict` | `Object` | `dict<K,V>` |

**Example:**

```naab
use io
use json

main {
    # NAAb → Python → NAAb type conversion
    let naab_list = [1, 2, 3, 4, 5]

    let python_result = <<python[naab_list]
    # naab_list is now a Python list
    sum(naab_list)  # Returns Python int
    >>

    # python_result is automatically converted back to NAAb int
    io.write("Sum: ", python_result, "\n")  # Sum: 15
}
```

---

## 4.5.8 Error Handling

Polyglot blocks can throw errors that propagate to NAAb:

```naab
use io

main {
    try {
        let result = <<python
        raise ValueError("Something went wrong!")
        >>
    } catch (e) {
        io.write("Caught error: ", e.message, "\n")
    }
}
```

**Best Practice:** Always use try/catch for operations that might fail (network, file I/O, etc.)

---

## 4.5.9 Performance Characteristics

### Parallel Execution

```naab
# Serial execution (10 seconds total)
let r1 = expensive_operation_1()  # 5 seconds
let r2 = expensive_operation_2()  # 5 seconds

# Parallel execution (~5 seconds total)
let r1 = <<python expensive_op_1() >>  # 5 seconds }
let r2 = <<python expensive_op_2() >>  # 5 seconds } in parallel
```

### When to Use Polyglot Async

**Good for:**
- ✅ I/O-bound operations (API calls, file I/O)
- ✅ CPU-intensive tasks (ML inference, data processing)
- ✅ Multiple independent operations
- ✅ Leveraging language-specific libraries

**Not optimal for:**
- ❌ Simple arithmetic (overhead of threading)
- ❌ Operations that must be sequential
- ❌ Very short operations (< 10ms)

---

## 4.5.10 Security Considerations

Polyglot async execution is security-hardened:

1. **Input Validation** - All inputs/outputs validated at FFI boundary
2. **Size Limits** - Polyglot blocks capped at 1MB
3. **Timeout Protection** - Configurable timeouts prevent infinite loops
4. **Sandbox Integration** - Respects sandbox permissions
5. **Thread Safety** - Proper GIL management (Python), mutex protection

**Example with Timeout:**

```naab
# If polyglot code runs too long, it times out
# Default timeout is configurable in runtime
let result = <<python
import time
time.sleep(1000)  # Will timeout after configured limit
>>
```

---

## 4.5.11 Advanced Patterns

### Pattern 1: Map-Reduce with Polyglot

```naab
use io

fn parallel_map(items: list<int>) -> list<int> {
    # Process each chunk in parallel
    let chunk1 = <<python[items]
    [x * 2 for x in items[0:len(items)//2]]
    >>

    let chunk2 = <<python[items]
    [x * 2 for x in items[len(items)//2:]]
    >>

    # Combine results
    return chunk1 + chunk2
}

main {
    let data = [1, 2, 3, 4, 5, 6, 7, 8]
    let result = parallel_map(data)
    io.write("Mapped: ", result, "\n")
}
```

### Pattern 2: Polyglot Library Wrapper

```naab
# Wrap Python library in NAAb function
fn fetch_weather(city: string) -> dict {
    return <<python[city]
    import requests
    response = requests.get(f'https://api.weather.com/{city}')
    response.json()
    >>
}

main {
    let weather = fetch_weather("London")
    io.write("Weather: ", weather, "\n")
}
```

### Pattern 3: Multi-Language Pipeline

```naab
fn data_pipeline(input_file: string) -> string {
    # 1. Extract with Python
    let raw = <<python[input_file]
    import pandas as pd
    pd.read_csv(input_file).to_json()
    >>

    # 2. Transform with JavaScript
    let transformed = <<javascript[raw]
    const data = JSON.parse(raw);
    // Complex transformations...
    JSON.stringify(processed)
    >>

    # 3. Analyze with Python+NumPy
    let analysis = <<python[transformed]
    import numpy as np
    # Statistical analysis...
    results.to_json()
    >>

    return analysis
}
```

---

## 4.5.12 Comparison: Polyglot vs Native Async

| Feature | Polyglot Async | Native async/await |
|---------|---------------|-------------------|
| **Status** | ✅ Available now | ⏸️ Planned for v2.0 |
| **Syntax** | `<<python code>>` | `async fn/await` |
| **Libraries** | ✅ Full ecosystem | ❌ Limited |
| **Parallelism** | ✅ True (OS threads) | ✅ True |
| **Use cases** | ML, APIs, data | Pure NAAb async |

**Note:** Native `async`/`await` syntax is planned for v2.0. For now, polyglot async provides all necessary parallel execution capabilities.

---

## 4.5.13 Best Practices

1. **Use for I/O-bound operations** - Network requests, file I/O, database queries
2. **Keep polyglot blocks focused** - One operation per block for clarity
3. **Handle errors** - Always use try/catch for operations that might fail
4. **Consider overhead** - Don't use for trivial operations
5. **Leverage language strengths** - Python for ML, JavaScript for JSON, Shell for system ops

---

## 4.5.14 Examples Gallery

Find complete working examples in:
- `examples/polyglot_async/parallel_api_calls.naab`
- `examples/polyglot_async/ml_inference.naab`
- `examples/polyglot_async/data_pipeline.naab`
- `examples/polyglot_async/web_scraping.naab`

---

## Summary

Polyglot async execution enables you to:
- ✅ Run multiple languages in parallel threads
- ✅ Leverage mature libraries (requests, pandas, numpy, etc.)
- ✅ Achieve true parallelism for I/O and CPU-bound tasks
- ✅ Build complex data pipelines
- ✅ Execute with security hardening and timeout protection

**Next Chapter:** [Chapter 5: Error Handling and Debugging](chapter05.md)
