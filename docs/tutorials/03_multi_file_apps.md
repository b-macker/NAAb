# Tutorial 3: Multi-File Applications

**Duration**: 20 minutes
**Prerequisites**: [Tutorial 2: First Application](02_first_application.md)
**What you'll learn**: Organize code across multiple files, use import/export

---

## Project Structure

We'll refactor the todo app into multiple modules:

```
todo-app/
├── main.naab           # Entry point
├── models/
│   └── todo.naab       # Todo data structure
├── storage/
│   └── file_store.naab # File I/O operations
└── ui/
    └── cli.naab        # User interface
```

---

## Step 1: Create Project Structure

```bash
mkdir -p ~/naab-projects/todo-app-multi/{models,storage,ui}
cd ~/naab-projects/todo-app-multi
```

---

## Step 2: Todo Model (`models/todo.naab`)

```naab
// Export todo creation function
export function create_todo(title) {
    return {
        "title": title,
        "completed": false,
        "created_at": timestamp()
    }
}

// Export validation
export function validate_title(title) {
    import "stdlib" as std

    if (std.length(title) == 0) {
        throw "Title cannot be empty"
    }

    if (std.length(title) > 100) {
        throw "Title too long (max 100 chars)"
    }

    return true
}

// Helper to check if todo is complete
export function is_complete(todo) {
    return todo["completed"]
}

// Helper to get todo summary
export function get_summary(todo) {
    let status = is_complete(todo) ? "[✓]" : "[ ]"
    return status + " " + todo["title"]
}
```

---

## Step 3: File Storage (`storage/file_store.naab`)

```naab
import "stdlib" as std

// Export save function
export function save_to_file(filename, data) {
    try {
        let json = std.stringify(data)
        std.write_file(filename, json)
        return true
    } catch (error) {
        print("Save error: " + error)
        return false
    }
}

// Export load function
export function load_from_file(filename) {
    try {
        if (!std.exists(filename)) {
            return []
        }

        let content = std.read_file(filename)
        return std.parse(content)
    } catch (error) {
        print("Load error: " + error)
        return []
    }
}

// Export backup function
export function backup_file(filename) {
    try {
        if (std.exists(filename)) {
            let backup_name = filename + ".backup"
            let content = std.read_file(filename)
            std.write_file(backup_name, content)
            return true
        }
    } catch (error) {
        print("Backup error: " + error)
    }
    return false
}
```

---

## Step 4: CLI Interface (`ui/cli.naab`)

```naab
import "stdlib" as std

// Export menu display
export function show_menu() {
    print("\n=== Todo App ===")
    print("1. List todos")
    print("2. Add todo")
    print("3. Complete todo")
    print("4. Search todos")
    print("5. Show stats")
    print("6. Save and exit")
    print("\nChoice: ")
}

// Export todo list display
export function display_todos(todos) {
    if (std.length(todos) == 0) {
        print("No todos yet!")
        return
    }

    print("\n=== Your Todos ===")
    let i = 0
    for (todo in todos) {
        print(i + ". " + get_summary(todo))
        i = i + 1
    }
    print("")
}

// Export stats display
export function display_stats(todos) {
    let total = std.length(todos)
    let completed = 0

    for (todo in todos) {
        if (todo["completed"]) {
            completed = completed + 1
        }
    }

    let pending = total - completed

    print("\n=== Stats ===")
    print("Total: " + total)
    print("Completed: " + completed)
    print("Pending: " + pending)

    if (total > 0) {
        let percent = (completed * 100) / total
        print("Progress: " + percent + "%")
    }
    print("")
}

// Export input helper
export function get_input(prompt) {
    print(prompt)
    return input()
}
```

---

## Step 5: Main Application (`main.naab`)

```naab
import {create_todo, validate_title, get_summary} from "./models/todo.naab"
import {save_to_file, load_from_file, backup_file} from "./storage/file_store.naab"
import {show_menu, display_todos, display_stats, get_input} from "./ui/cli.naab"
import "stdlib" as std

let FILENAME = "todos.json"
let todos = []

// Business logic functions
function add_todo(title) {
    validate_title(title)  // Throws on invalid
    let todo = create_todo(title)
    todos = std.push(todos, todo)
    print("✓ Added: " + title)
}

function complete_todo(index) {
    if (index < 0 || index >= std.length(todos)) {
        throw "Invalid index"
    }

    todos[index]["completed"] = true
    print("✓ Completed: " + todos[index]["title"])
}

function search_todos(keyword) {
    let results = []
    for (todo in todos) {
        if (std.contains(todo["title"], keyword)) {
            results = std.push(results, todo)
        }
    }
    return results
}

// Main program loop
function main() {
    print("Loading todos...")
    todos = load_from_file(FILENAME)
    print("Loaded " + std.length(todos) + " todos")

    let running = true
    while (running) {
        show_menu()
        let choice = get_input("")

        try {
            if (choice == "1") {
                display_todos(todos)
            } else if (choice == "2") {
                let title = get_input("Enter todo title: ")
                add_todo(title)
            } else if (choice == "3") {
                display_todos(todos)
                let index = get_input("Enter index: ")
                complete_todo(index)
            } else if (choice == "4") {
                let keyword = get_input("Search for: ")
                let results = search_todos(keyword)
                print("\nSearch Results:")
                display_todos(results)
            } else if (choice == "5") {
                display_stats(todos)
            } else if (choice == "6") {
                backup_file(FILENAME)
                save_to_file(FILENAME, todos)
                print("Saved. Goodbye!")
                running = false
            } else {
                print("Invalid choice")
            }
        } catch (error) {
            print("Error: " + error)
        }
    }
}

main()
```

---

## Step 6: Run the Application

```bash
cd ~/naab-projects/todo-app-multi
~/naab-lang run main.naab
```

---

## Benefits of Multi-File Structure

### 1. **Separation of Concerns**
- Models: Data structures and validation
- Storage: Persistence logic
- UI: Display and interaction
- Main: Business logic and orchestration

### 2. **Reusability**
```naab
// Use the same storage module in different apps
import {save_to_file, load_from_file} from "../shared/storage/file_store.naab"
```

### 3. **Testability**
```naab
// Test models independently
import {create_todo, validate_title} from "./models/todo.naab"

function test_validation() {
    try {
        validate_title("")
        print("FAIL: Should reject empty title")
    } catch (error) {
        print("PASS: Empty title rejected")
    }
}
```

### 4. **Maintainability**
- Each file has single responsibility
- Easier to find and fix bugs
- Changes isolated to specific modules

---

## Advanced Import Patterns

### Wildcard Import
```naab
import * as todo_model from "./models/todo.naab"

let new_todo = todo_model.create_todo("Task")
let is_valid = todo_model.validate_title("Task")
```

### Aliasing
```naab
import {save_to_file as save, load_from_file as load} from "./storage/file_store.naab"

let data = load("data.json")
save("output.json", data)
```

### Selective Import
```naab
import {create_todo, validate_title} from "./models/todo.naab"
// Only imports what you need
```

---

## Module Search Paths

NAAb searches for modules in this order:

1. **Relative to current file**: `./models/todo.naab`
2. **naab_modules/ in current dir**: `naab_modules/package/file.naab`
3. **naab_modules/ in parent dirs**: `../naab_modules/...`
4. **Global modules**: `~/.naab/modules/...`
5. **System modules**: `/usr/local/naab/modules/...`

---

## Best Practices

### 1. Use Consistent Naming
```naab
// Good
models/user.naab
models/post.naab
storage/db.naab
ui/components.naab

// Bad
User.naab
POST.naab
database_stuff.naab
```

### 2. Export Only Public API
```naab
// Internal helper (not exported)
function internal_helper(x) {
    return x * 2
}

// Public API (exported)
export function public_function(x) {
    return internal_helper(x) + 1
}
```

### 3. Avoid Circular Dependencies
```naab
// Bad - circular import
// a.naab imports b.naab
// b.naab imports a.naab

// Good - extract common code
// a.naab and b.naab both import common.naab
```

### 4. Document Module Interface
```naab
// models/todo.naab
/**
 * Todo model module
 * Exports: create_todo, validate_title, is_complete
 */

export function create_todo(title) {
    // Creates a new todo item
    // Args: title (string)
    // Returns: todo object
}
```

---

## Next Steps

- **Tutorial 4**: [Block Integration](04_block_integration.md)
- **Cookbook**: [../COOKBOOK.md](../COOKBOOK.md)
- **Architecture**: [../ARCHITECTURE.md](../ARCHITECTURE.md)

---

**Congratulations!** You can now organize NAAb code into maintainable multi-file applications.
