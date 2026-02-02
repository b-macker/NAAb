# Chapter 9: System Interaction

Interacting with the underlying operating system is essential for any practical application. NAAb provides a set of native modules to handle input/output, file system operations, time management, and environment variables efficiently.

## 9.1 The `io` Module and Output

For basic output, NAAb provides the global `print` function, which writes to standard output with a newline.

```naab
main {
    print("Hello, World!")
}
```

### 9.1.1 Advanced Console I/O

For more control, the `io` module provides functions for writing without newlines, writing to stderr, and reading input. The `io` module must be brought into scope using a `use` statement.

```naab
use io as io

main {
    io.write("Enter your name: ")
    let name = io.read_line()
    io.write("Hello, ", name, "!\n")
    
    // Write to standard error
    io.write_error("Warning: System running in test mode.\n")
}
```

## 9.2 The `file` Module: File System Operations

The `file` module allows you to read from and write to files on the disk. It must be brought into scope using a `use` statement, typically aliased as `fs`.

```naab
use file as fs

main {
    let filename = "example.txt"
    let content = "This is a test file created by NAAb.\nSecond line."

    // Write content to a file (overwrites existing)
    fs.write(filename, content)
    print("File written:", filename)

    // Check if file exists
    if fs.exists(filename) {
        print("File exists!")
    }

    // Read content back
    let read_content = fs.read(filename)
    print("File content:")
    print(read_content)

    // Append to file
    fs.append(filename, "\nThird line appended.")
    
    // Delete file
    fs.delete(filename)
    print("File deleted.")
}
```

## 9.3 The `time` Module: Time and Scheduling

The `time` module provides functions for getting the current time, measuring duration (benchmarking), and pausing execution. It must be brought into scope using a `use` statement.

```naab
use time as time

main {
    // Current Unix timestamp (seconds)
    let now = time.now()
    print("Current timestamp:", now)

    // Current time in milliseconds (good for benchmarking)
    let start = time.now_millis()
    
    // Formatting time
    let formatted = time.format_timestamp(now, "%Y-%m-%d %H:%M:%S")
    print("Current time:", formatted)

    // Getting date components
    print("Year:", time.year(now))
    print("Month:", time.month(now))
    print("Day:", time.day(now))
    print("Hour:", time.hour(now))

    // Sleep (pause execution) for 0.5 seconds
    print("Sleeping...")
    time.sleep(0.5)
    print("Woke up!")

    let end = time.now_millis()
    print("Execution took:", end - start, "ms")
}
```

## 9.4 The `env` Module: Environment Variables

The `env` module allows you to access and modify environment variables, which is useful for configuration and system integration. It must be brought into scope using a `use` statement.

```naab
use env as env

main {
    // Get an environment variable
    let path = env.get("PATH")
    // print("PATH:", path) 

    // Set a new environment variable (for this process)
    env.set_var("MY_APP_MODE", "production")
    print("MY_APP_MODE set to:", env.get("MY_APP_MODE"))

    // Check if variable is set (get returns empty string or default if missing)
    let missing = env.get("NON_EXISTENT_VAR", "default_value")
    print("Missing var (with default):", missing)
}
```

### 9.4.1 Command-Line Arguments

The `env` module also provides access to command-line arguments passed to your script via `env.get_args()`. This function returns a `list<string>` containing all non-flag arguments passed after the script name.

**Important:** CLI flags like `--verbose`, `--profile`, `--explain`, and `--no-color` are automatically filtered out and will NOT appear in the script arguments list.

```naab
use io
use json
use env
use array

main {
    // Get all command-line arguments
    let args = env.get_args()
    let count = array.length(args)

    io.write("Number of arguments: ", json.stringify(count), "\n")
    io.write("Arguments: ", json.stringify(args), "\n")

    // Access individual arguments
    if count > 0 {
        io.write("First argument: ", args[0], "\n")
    }

    // Iterate over arguments
    for arg in args {
        io.write("  - ", arg, "\n")
    }
}
```

**Usage examples:**

```bash
# No arguments
./build/naab-lang run script.naab
# args = []

# With arguments
./build/naab-lang run script.naab arg1 arg2 arg3
# args = ["arg1", "arg2", "arg3"]

# Arguments with spaces
./build/naab-lang run script.naab "hello world" test
# args = ["hello world", "test"]

# Flags are filtered out
./build/naab-lang run script.naab --verbose data.txt --profile output.txt
# args = ["data.txt", "output.txt"]
# (--verbose and --profile are not included)
```

**Common use cases:**

```naab
use io
use env
use array

main {
    let args = env.get_args()
    let count = array.length(args)

    // Simple file processor
    if count == 0 {
        io.write("Usage: script.naab <input_file> [output_file]\n")
        return
    }

    let input_file = args[0]
    let output_file = if count > 1 { args[1] } else { "output.txt" }

    io.write("Processing: ", input_file, " -> ", output_file, "\n")
    // ... process files ...
}
```