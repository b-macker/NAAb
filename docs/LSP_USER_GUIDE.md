# NAAb LSP User Guide

## Overview

The NAAb Language Server Protocol (LSP) implementation provides rich IDE features for the NAAb programming language, including autocomplete, hover information, go-to-definition, and real-time diagnostics.

## Features

### 1. Autocomplete (Code Completion)

**What it does:** Provides intelligent code suggestions as you type.

**How to use:**
- Type any code in a `.naab` file
- Press `Ctrl+Space` (or wait for automatic trigger)
- Select from the completion list

**What you get:**
- **Keywords**: `fn`, `let`, `if`, `for`, `while`, `struct`, `enum`, etc.
- **Functions**: All defined functions with signatures
- **Variables**: All variables in scope
- **Types**: `int`, `string`, `bool`, `float`, `void`, `list`, `dict`, etc.

**Example:**
```naab
fn greet(name: string) -> string {
    return "Hello"
}

main {
    let x = g|  // Press Ctrl+Space here
    // Shows: greet, let, if, for, etc.
}
```

### 2. Hover Information

**What it does:** Shows type information and function signatures when you hover over symbols.

**How to use:**
- Hover your mouse over any function, variable, or type
- Wait briefly for the hover popup

**What you get:**
- Variable types: `let x: int`
- Function signatures: `fn add(a: int, b: int) -> int`
- Struct definitions: `struct Point`

**Example:**
```naab
let myVar: int = 42

// Hover over "myVar" shows:
// ```naab
// let myVar: int
// ```
```

### 3. Go-to-Definition

**What it does:** Jumps to where a symbol is defined.

**How to use:**
- Ctrl+Click (or F12) on any function name, variable, or type
- Editor jumps to the definition location

**Example:**
```naab
fn calculate() -> int {
    return 42
}

main {
    let x = calculate()  // Ctrl+Click on "calculate"
    // Jumps to line 1 where function is defined
}
```

### 4. Document Symbols (Outline)

**What it does:** Shows a hierarchical outline of all symbols in the file.

**How to use:**
- Press `Ctrl+Shift+O` (VS Code)
- Or open the "Outline" panel in the sidebar

**What you see:**
- All functions
- All structs with their fields
- All enums with their variants
- Main block

**Example outline:**
```
📄 example.naab
├── 🔧 add(a: int, b: int) -> int
├── 🔧 greet(name: string) -> string
├── 📦 Point
│   ├── x: int
│   └── y: int
└── 🔧 main
```

### 5. Real-time Diagnostics

**What it does:** Shows errors and warnings as you type.

**What you get:**
- **Parse errors**: Red squiggles for syntax errors
- **Type errors**: Red squiggles for type mismatches
- **Error messages**: Detailed descriptions with line numbers

**Example:**
```naab
main {
    let x: int = "hello"  // Red squiggle
    // Error: Type error: expected int, got string
}
```

## Installation

### Prerequisites

- NAAb compiler installed
- NAAb LSP server (`naab-lsp`) built and in PATH

### Building the LSP Server

```bash
cd ~/.naab/language
cmake --build build --target naab-lsp
sudo cp build/naab-lsp /usr/local/bin/  # Or add to PATH
```

### Verify Installation

```bash
which naab-lsp
# Should output: /usr/local/bin/naab-lsp

naab-lsp --version  # (if implemented)
```

## Editor Setup

### VS Code

1. **Install Extension:**
   ```bash
   cd vscode-naab
   npm install
   npm run compile
   npx vsce package
   code --install-extension naab-0.1.0.vsix
   ```

2. **Configure Settings:**
   - Open Settings (Ctrl+,)
   - Search for "naab"
   - Set `naab.lsp.serverPath` if needed

3. **Verify:**
   - Open a `.naab` file
   - Check status bar for "NAAb" indicator
   - Try autocomplete (Ctrl+Space)

### Neovim

Add to your Neovim config (Lua):

```lua
require('lspconfig').configs.naab = {
  default_config = {
    cmd = {'naab-lsp'},
    filetypes = {'naab'},
    root_dir = function(fname)
      return vim.fn.getcwd()
    end,
  },
}

require('lspconfig').naab.setup{}
```

Verify:
```vim
:LspInfo  " Should show naab-lsp attached
```

### Emacs (lsp-mode)

Add to your Emacs config:

```elisp
(require 'lsp-mode)

(add-to-list 'lsp-language-id-configuration '(naab-mode . "naab"))

(lsp-register-client
 (make-lsp-client
  :new-connection (lsp-stdio-connection "naab-lsp")
  :major-modes '(naab-mode)
  :server-id 'naab-lsp))

(add-hook 'naab-mode-hook #'lsp)
```

## Configuration

### Server Settings

The LSP server can be configured via command-line flags or environment variables:

```bash
# Example: Set log level
export NAAB_LSP_LOG_LEVEL=debug
naab-lsp

# Example: Disable debouncing (not recommended)
export NAAB_LSP_NO_DEBOUNCE=1
naab-lsp
```

### Client Settings (VS Code)

```json
{
  "naab.lsp.serverPath": "/path/to/naab-lsp",
  "naab.trace.server": "verbose"  // For debugging
}
```

## Troubleshooting

### Issue: LSP Server Not Starting

**Symptoms:**
- No autocomplete
- No diagnostics
- Editor shows "LSP not connected"

**Solutions:**

1. **Check server is installed:**
   ```bash
   which naab-lsp
   ```

2. **Check server runs:**
   ```bash
   echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | naab-lsp
   # Should output JSON response
   ```

3. **Check editor logs:**
   - VS Code: Output panel → "NAAb Language Server"
   - Neovim: `:LspLog`
   - Emacs: `*lsp-log*` buffer

4. **Enable verbose logging:**
   ```json
   {
     "naab.trace.server": "verbose"
   }
   ```

### Issue: Autocomplete Not Working

**Possible causes:**
1. File not saved with `.naab` extension
2. Syntax errors in code
3. LSP server crashed

**Solutions:**
1. Save file as `*.naab`
2. Fix syntax errors (check diagnostics)
3. Restart editor

### Issue: Slow Performance

**Possible causes:**
1. Very large file (>10,000 lines)
2. Many symbols (>1000 functions/variables)

**Solutions:**
1. Split large files
2. Reduce symbol count
3. Disable features selectively

### Issue: Diagnostics Not Updating

**Possible causes:**
1. Debouncing delay (300ms)
2. Server crashed
3. Parser error

**Solutions:**
1. Wait 300ms after typing
2. Check server logs
3. Restart LSP server

## Performance Tips

### Debouncing

The LSP server debounces diagnostics by 300ms. This means:
- **Fast typing**: Diagnostics update after you stop typing
- **Slow typing**: May see multiple updates
- **Benefit**: Reduced CPU usage during rapid typing

### Caching

Completion results are cached. This means:
- **First request**: ~5ms
- **Repeated request**: <1ms
- **Cache invalidation**: Automatic on document change

### Best Practices

1. **Keep files under 5,000 lines** for best performance
2. **Use incremental saves** (not save-on-every-keystroke)
3. **Close unused files** to reduce memory usage
4. **Disable unused features** if performance is critical

## Known Limitations

### Current Limitations

1. **Multi-file Support**
   - Can only jump to definitions in same file
   - No cross-file symbol resolution
   - Workaround: Open both files, use search

2. **Member Access Completions**
   - `obj.` completions not fully implemented
   - Shows empty list for struct fields
   - Workaround: Type field name manually

3. **Function Parameter Scope**
   - Parameters treated as `any` in function body
   - Doesn't affect completion or hover
   - Workaround: Use explicit types

4. **Find References**
   - Not yet implemented
   - Use editor's text search instead

5. **Rename Refactoring**
   - Not yet implemented
   - Use editor's find-and-replace

### Planned Features (Future)

- Multi-file workspace support
- Find all references
- Rename symbol refactoring
- Code actions (quick fixes)
- Semantic highlighting
- Inlay hints (inline type annotations)

## FAQ

### Q: Why is autocomplete slow?

**A:** First request computes completions (~5ms). Subsequent requests use cache (<1ms). If always slow, check for syntax errors or large files.

### Q: Can I use this with [editor]?

**A:** Yes! The LSP server follows the standard LSP protocol. Any editor with LSP support can use it (VS Code, Neovim, Emacs, Sublime, Vim, etc.).

### Q: How do I update the extension?

**A:**
```bash
cd vscode-naab
git pull
npm install
npm run compile
npx vsce package
code --install-extension naab-0.1.0.vsix --force
```

### Q: Can I disable specific features?

**A:** Not currently supported. All features are enabled by default.

### Q: Does this work offline?

**A:** Yes! The LSP server is local and doesn't require internet.

### Q: How much memory does it use?

**A:** Typical usage: 10-50 MB RAM. Increases with file size and number of symbols.

## Getting Help

### Resources

- **Documentation**: `docs/` folder in NAAb repository
- **Issues**: https://github.com/naab-lang/naab/issues
- **Discussions**: https://github.com/naab-lang/naab/discussions

### Reporting Bugs

When reporting issues, include:
1. NAAb version: `naab --version`
2. LSP server version: `naab-lsp --version`
3. Editor and version
4. Minimal reproduction code
5. LSP server logs (enable verbose mode)

### Example Bug Report

```
Title: Autocomplete not showing variables

Environment:
- NAAb: 0.1.0
- naab-lsp: 0.1.0
- Editor: VS Code 1.70.0
- OS: Ubuntu 22.04

Steps to reproduce:
1. Create file: test.naab
2. Type: main { let x = 42 }
3. Press Ctrl+Space after "let "
4. Expected: Shows "x" in completions
5. Actual: Shows only keywords

Logs:
[CompletionProvider] Getting completions at line 0, char 10
[CompletionProvider] Symbol table has 0 symbols
```

## Advanced Usage

### Custom LSP Server Path

If `naab-lsp` is not in PATH:

**VS Code:**
```json
{
  "naab.lsp.serverPath": "/home/user/naab/build/naab-lsp"
}
```

**Neovim:**
```lua
require('lspconfig').naab.setup{
  cmd = {'/home/user/naab/build/naab-lsp'}
}
```

### Development Mode

For developing the LSP server:

```bash
# Build with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build --target naab-lsp

# Run with verbose logging
./build/naab-lsp 2>&1 | tee lsp.log
```

### Testing Protocol Manually

```bash
# Test initialize
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///test"}}' | ./build/naab-lsp

# Test completion
echo 'Content-Length: 150\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///test.naab"},"position":{"line":0,"character":5}}}' | ./build/naab-lsp
```

---

**Last Updated:** February 3, 2026
**Version:** 0.1.0
