# NAAb Tutorial Files

Complete guide to learning NAAb syntax and polyglot programming!

## 📚 Available Tutorial Files

### 1. GETTING_STARTED.naab
**Best for:** Complete beginners
**Duration:** 10 minutes
**Covers:**
- Hello World
- Variables and types
- Lists and dictionaries
- Control flow (if/else, loops)
- Functions
- Your first polyglot block
- Variable binding
- Return values
- Combining NAAb + Python
- Standard library basics

**Run it:**
```bash
cd ~/.naab/language/build
./naab-lang run ../GETTING_STARTED.naab
```

### 2. QUICK_REFERENCE.naab
**Best for:** Quick syntax lookup
**Duration:** 5 minutes
**Covers:**
- Basic syntax summary
- All polyglot block patterns
- Variable binding examples
- Return value examples
- All 8 supported languages
- Real-world examples
- Best practices
- Syntax cheat sheet

**Run it:**
```bash
cd ~/.naab/language/build
./naab-lang run ../QUICK_REFERENCE.naab
```

### 3. TUTORIAL_POLYGLOT_BLOCKS.naab
**Best for:** Advanced users wanting deep dive
**Duration:** 20 minutes
**Covers:**
- 10 comprehensive sections
- Basic NAAb syntax
- Inline code blocks
- Variable binding (detailed)
- Return values (detailed)
- Combining variables + returns
- Real-world examples
- Multi-language mixing
- Advanced patterns
- All supported languages
- Best practices

**Run it:**
```bash
cd ~/.naab/language/build
./naab-lang run ../TUTORIAL_POLYGLOT_BLOCKS.naab
```

### 4. TUTORIAL_BLOCK_ASSEMBLY.naab ⭐ NEW!
**Best for:** Users who want to use pre-built blocks
**Duration:** 15 minutes
**Covers:**
- Block Assembly System (see `../reference/BLOCK_ASSEMBLY.md` for setup and CLI commands)
- Searching for blocks
- Listing available blocks
- Using blocks in your code
- Block composition (chaining)
- Validating block compatibility
- Inspecting block metadata
- Block categories
- Practical examples
- Creating your own blocks
- Troubleshooting


**Run it:**
```bash
cd ~/.naab/language/build
./naab-lang run ../TUTORIAL_BLOCK_ASSEMBLY.naab
```

## 🚀 Quick Start (30 seconds)

```bash
# Navigate to build directory
cd ~/.naab/language/build

# Run beginner tutorial
./naab-lang run ../GETTING_STARTED.naab

# Read the output and learn!
```

## 📖 Learning Path

**Day 1:** Run GETTING_STARTED.naab
- Learn basic syntax
- Try your first polyglot block
- Understand variable binding

**Day 2:** Run QUICK_REFERENCE.naab
- Review syntax patterns
- See all language examples
- Learn best practices

**Day 3:** Run TUTORIAL_POLYGLOT_BLOCKS.naab
- Deep dive into advanced features
- See real-world examples
- Master polyglot programming

**Day 4:** Run TUTORIAL_BLOCK_ASSEMBLY.naab ⭐ NEW!
- **FIRST:** Refer to `../reference/BLOCK_ASSEMBLY.md` for initial setup (`blocks index`)
- Discover the 585+ block library (and growing!)
- Search and use pre-built blocks
- Chain blocks together
- Validate block compatibility

**Day 5:** Write your own program!
- Start with a simple task
- Use blocks from the registry
- Write custom inline code
- Combine everything you learned

## 💡 Key Concepts

### Basic Block Syntax
```naab
# Simple inline code
<<python
print("Hello from Python!")
>>
```

### Variable Binding
```naab
# Pass NAAb variables to blocks
let name = "Alice"
<<python[name]
print(f"Hello, {name}!")
>>
```

### Return Values
```naab
# Get results from blocks
let result = <<python
2 + 2
>>
print(result)  # 4
```

### Combined (Variables + Return)
```naab
let numbers = [1, 2, 3, 4, 5]
let doubled = <<python[numbers]
[x * 2 for x in numbers]
>>
print(doubled)  # [2, 4, 6, 8, 10]
```

### Block Assembly (Pre-built Blocks)
Refer to the [Block Assembly System guide](../reference/BLOCK_ASSEMBLY.md) for detailed commands and usage.

## 🎯 What You'll Learn

### NAAb Syntax
- ✅ Variables and type inference
- ✅ Lists and dictionaries
- ✅ Array element assignment (NEW!)
- ✅ Control flow (if/else, loops)
- ✅ Functions
- ✅ Structs (optional)
- ✅ Error handling (try/catch)

### Polyglot Features
- ✅ 8 supported languages
- ✅ Inline code blocks
- ✅ Variable binding
- ✅ Return values
- ✅ Multi-language mixing
- ✅ Type conversions

### Block Assembly System ⭐ NEW!
- ✅ 24,515+ pre-built blocks
- ✅ Search and discover blocks
- ✅ Block composition (chaining)
- ✅ Type validation
- ✅ Cross-language blocks
- ✅ Create your own blocks

### Standard Library
- ✅ array module (map, filter, reduce, sort)
- ✅ string module (upper, lower, split, join)
- ✅ math module (sqrt, pow, trig functions)
- ✅ json module (parse, stringify)
- ✅ http module (GET, POST, PUT, DELETE)
- ✅ io module (read, write, append)
- ✅ Plus 7 more modules!

## 🔧 Testing Your Own Code

Create a new file:
```bash
cd ~/.naab/language
nano my_first_program.naab
```

Write some code:
```naab
main {
    print("My first NAAb program!")

    let name = "Your Name"
    let age = 25

    <<python[name, age]
    print(f"Hello {name}, you are {age} years old")
    >>
}
```

Run it:
```bash
cd build
./naab-lang run ../my_first_program.naab
```

## 📊 Supported Languages

| Language | Keyword | Example |
|----------|---------|---------|
| Python | `python` | `<<python print("Hi") >>` |
| JavaScript | `javascript` | `<<javascript console.log("Hi") >>` |
| Bash | `bash` or `shell` | `<<bash echo "Hi" >>` |
| C++ | `cpp` | `<<cpp std::cout << "Hi" >>` |
| Rust | `rust` | `<<rust println!("Hi") >>` |
| Ruby | `ruby` | `<<ruby puts "Hi" >>` |
| Go | `go` | `<<go fmt.Println("Hi") >>` |
| C# | `csharp` | `<<csharp Console.WriteLine("Hi") >>` |

## ⚡ Performance Tips

**Use NAAb stdlib when possible (10-100x faster!):**
```naab
use array

# FAST - Native C++
let sorted = array.sort(numbers)

# SLOW - Polyglot overhead
let sorted = <<python[numbers]
sorted(numbers)
>>
```

**Batch operations:**
```naab
# GOOD - Single block
let results = <<python[data]
[process(item) for item in data]
>>

# BAD - Multiple blocks
for item in data {
    let result = <<python[item]
    process(item)
    >>
}
```

## 🐛 Common Issues

### Issue: "Module not found"
```naab
# Wrong:
use list  # No 'list' module

# Right:
use array  # Use 'array' module
```

### Issue: "Dictionary key not found"
```naab
# Check key exists first:
if dict.contains(person, "age") {
    print(person["age"])
}

# Or use assignment to create:
person["age"] = "30"  # Creates if missing
```

### Issue: "List index out of bounds"
```naab
# Check bounds:
if i < array.length(my_list) {
    my_list[i] = value
}
```

## 📁 File Locations

All tutorial files are in:
```
~/.naab/language/docs/tutorials/
├── GETTING_STARTED.naab          # Beginner tutorial
├── QUICK_REFERENCE.naab          # Syntax cheat sheet
└── TUTORIAL_POLYGLOT_BLOCKS.naab # Advanced tutorial
```

Other guides:
```
~/.naab/language/docs/guides/README_TUTORIALS.md  # This file
~/.naab/language/docs/reference/BLOCK_ASSEMBLY.md # Block Assembly Reference
~/.naab/docs/AI_ASSISTANT_GUIDE.md                # Complete AI reference
~/.naab/language/MASTER_STATUS.md                 # Project status
```

## 🎓 Next Steps

After completing the tutorials:

1. **Read the full guide:**
   ```bash
   cat ~/.naab/docs/AI_ASSISTANT_GUIDE.md
   ```

2. **Check project status:**
   ```bash
   cat ~/.naab/language/MASTER_STATUS.md | head -50
   ```

3. **Run example tests:**
   ```bash
   cd ~/.naab/language/build
   ./naab-lang run ../examples/test_simple_inference.naab
   ./naab-lang run ../test_array_assignment.naab
   ```

4. **Explore benchmarks:**
   ```bash
   ./naab-lang run ../benchmarks/macro/fibonacci.naab
   ./naab-lang run ../benchmarks/macro/sorting.naab
   ```

5. **Write your own projects!**

## 📞 Need Help?

- Read AI_ASSISTANT_GUIDE.md for complete reference
- Check MASTER_STATUS.md for current features
- Look at examples/ directory for more code
- Check docs/sessions/ for implementation details

## ✅ What's New (2026-01-20)

- ✅ **Array element assignment** - `arr[i] = value` now works!
- ✅ **Dictionary assignment** - `dict[key] = value` creates or updates
- ✅ **Sorting algorithms** - Bubble sort and others now possible
- ✅ **All in-place algorithms** - Matrices, graphs, etc. unblocked

## 🎉 Have Fun Learning NAAb!

Start with GETTING_STARTED.naab and work your way up. By the end, you'll be mixing Python, JavaScript, and other languages seamlessly in your NAAb programs!

**Happy coding! 🚀**
