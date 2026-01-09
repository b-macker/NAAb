# Tutorial 2: Building Your First Application

**Duration**: 30 minutes
**Prerequisites**: [Tutorial 1: Getting Started](01_getting_started.md)
**What you'll build**: A todo list manager with file persistence

---

## Project Overview

We'll build a command-line todo list application with these features:
- Add tasks
- List tasks
- Mark tasks as complete
- Save/load from file

---

## Step 1: Project Setup

Create project directory:

```bash
mkdir -p ~/naab-projects/todo-app
cd ~/naab-projects/todo-app
```

---

## Step 2: Core Data Structure

Create `todo.naab`:

```naab
import "stdlib" as std

// Todo item structure (using dict)
function create_todo(title) {
    return {
        "title": title,
        "completed": false,
        "created_at": std.timestamp()
    }
}

// Todo list (array of dicts)
let todos = []

// Add todo
function add_todo(title) {
    let todo = create_todo(title)
    todos = std.push(todos, todo)
    print("Added: " + title)
}

// List all todos
function list_todos() {
    if (std.length(todos) == 0) {
        print("No todos yet!")
        return
    }

    print("\n=== Your Todos ===")
    let i = 0
    for (todo in todos) {
        let status = "[ ]"
        if (todo["completed"]) {
            status = "[✓]"
        }
        print(i + ". " + status + " " + todo["title"])
        i = i + 1
    }
    print("")
}

// Mark todo as complete
function complete_todo(index) {
    if (index < 0 || index >= std.length(todos)) {
        print("Invalid index")
        return
    }

    todos[index]["completed"] = true
    print("Completed: " + todos[index]["title"])
}

// Test it
add_todo("Learn NAAb")
add_todo("Build an app")
add_todo("Deploy to production")

list_todos()

complete_todo(0)
list_todos()
```

Run it:

```bash
~/naab-lang run todo.naab
```

Output:
```
Added: Learn NAAb
Added: Build an app
Added: Deploy to production

=== Your Todos ===
0. [ ] Learn NAAb
1. [ ] Build an app
2. [ ] Deploy to production

Completed: Learn NAAb

=== Your Todos ===
0. [✓] Learn NAAb
1. [ ] Build an app
2. [ ] Deploy to production
```

---

## Step 3: File Persistence

Update `todo.naab` to add save/load:

```naab
import "stdlib" as std

let FILENAME = "todos.json"

// Save todos to file
function save_todos() {
    try {
        let json = std.stringify(todos)
        std.write_file(FILENAME, json)
        print("Saved to " + FILENAME)
    } catch (error) {
        print("Failed to save: " + error)
    }
}

// Load todos from file
function load_todos() {
    try {
        if (std.exists(FILENAME)) {
            let content = std.read_file(FILENAME)
            todos = std.parse(content)
            print("Loaded " + std.length(todos) + " todos")
        }
    } catch (error) {
        print("Failed to load: " + error)
        todos = []
    }
}

// Load on startup
load_todos()

// Add your functions here...
// (create_todo, add_todo, list_todos, complete_todo)

// Example usage
add_todo("Write documentation")
add_todo("Fix bugs")

list_todos()

// Save on exit
save_todos()
```

---

## Step 4: Interactive Mode

Create `todo_cli.naab`:

```naab
import "stdlib" as std

// ... (include all functions from above)

function show_menu() {
    print("\n=== Todo App ===")
    print("1. List todos")
    print("2. Add todo")
    print("3. Complete todo")
    print("4. Save and exit")
    print("\nChoice: ")
}

function main() {
    load_todos()

    let running = true
    while (running) {
        show_menu()
        let choice = input()  // Get user input

        if (choice == "1") {
            list_todos()
        } else if (choice == "2") {
            print("Enter todo title: ")
            let title = input()
            add_todo(title)
        } else if (choice == "3") {
            print("Enter todo index: ")
            let index = input()
            complete_todo(index)
        } else if (choice == "4") {
            save_todos()
            print("Goodbye!")
            running = false
        } else {
            print("Invalid choice")
        }
    }
}

main()
```

---

## Step 5: Add Search and Filter

Add these functions:

```naab
// Search todos by keyword
function search_todos(keyword) {
    let results = []
    for (todo in todos) {
        if (std.contains(todo["title"], keyword)) {
            results = std.push(results, todo)
        }
    }
    return results
}

// Get only incomplete todos
function get_pending() {
    let pending = []
    for (todo in todos) {
        if (!todo["completed"]) {
            pending = std.push(pending, todo)
        }
    }
    return pending
}

// Get completed todos
function get_completed() {
    let completed = []
    for (todo in todos) {
        if (todo["completed"]) {
            completed = std.push(completed, todo)
        }
    }
    return completed
}

// Count stats
function show_stats() {
    let total = std.length(todos)
    let done = std.length(get_completed())
    let pending = total - done

    print("\n=== Stats ===")
    print("Total: " + total)
    print("Done: " + done)
    print("Pending: " + pending)

    if (total > 0) {
        let percent = (done * 100) / total
        print("Progress: " + percent + "%")
    }
}
```

---

## Step 6: Error Handling and Validation

Add validation:

```naab
function add_todo_safe(title) {
    // Validate input
    if (std.length(title) == 0) {
        print("Error: Title cannot be empty")
        return
    }

    if (std.length(title) > 100) {
        print("Error: Title too long (max 100 chars)")
        return
    }

    // Check for duplicates
    for (todo in todos) {
        if (todo["title"] == title) {
            print("Error: Todo already exists")
            return
        }
    }

    // Add todo
    let todo = create_todo(title)
    todos = std.push(todos, todo)
    print("✓ Added: " + title)
}

function complete_todo_safe(index) {
    try {
        if (index < 0 || index >= std.length(todos)) {
            throw "Index out of range"
        }

        if (todos[index]["completed"]) {
            print("Warning: Already completed")
            return
        }

        todos[index]["completed"] = true
        print("✓ Completed: " + todos[index]["title"])
    } catch (error) {
        print("Error: " + error)
    }
}
```

---

## Step 7: Testing

Create `test_todo.naab`:

```naab
import "stdlib" as std

// Test functions
function test_create_todo() {
    let todo = create_todo("Test task")
    if (todo["title"] != "Test task") {
        print("FAIL: create_todo")
    } else if (todo["completed"] != false) {
        print("FAIL: create_todo - completed should be false")
    } else {
        print("PASS: create_todo")
    }
}

function test_add_todo() {
    let initial_count = std.length(todos)
    add_todo("Test item")
    let new_count = std.length(todos)

    if (new_count == initial_count + 1) {
        print("PASS: add_todo")
    } else {
        print("FAIL: add_todo")
    }
}

function test_complete_todo() {
    add_todo("Complete me")
    let index = std.length(todos) - 1
    complete_todo(index)

    if (todos[index]["completed"] == true) {
        print("PASS: complete_todo")
    } else {
        print("FAIL: complete_todo")
    }
}

// Run tests
print("Running tests...")
test_create_todo()
test_add_todo()
test_complete_todo()
print("Tests complete!")
```

Run tests:

```bash
~/naab-lang run test_todo.naab
```

---

## Complete Application

Full `todo_app.naab`:

```naab
import "stdlib" as std

let FILENAME = "todos.json"
let todos = []

// ... (All functions above)

function main() {
    load_todos()
    show_stats()

    // Interactive mode
    let running = true
    while (running) {
        show_menu()
        // Handle user input...
    }
}

main()
```

---

## Enhancements

Try adding these features:

1. **Due Dates**: Add `due_date` field to todos
2. **Priorities**: Add `priority` (high/medium/low)
3. **Tags**: Add `tags` array for categorization
4. **Export**: Export to CSV or markdown
5. **Backup**: Auto-backup before saving
6. **Archive**: Move completed todos to archive file

---

## Next Steps

- **Tutorial 3**: [Multi-File Applications](03_multi_file_apps.md)
- **Tutorial 4**: [Block Integration](04_block_integration.md)
- **Cookbook**: [../COOKBOOK.md](../COOKBOOK.md) for more examples

---

**Congratulations!** You've built a complete todo application with file persistence, error handling, and tests.
