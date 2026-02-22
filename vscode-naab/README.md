# NAAb Language Support for VS Code

Language support for NAAb (NAAb Assembles Anything in Blocks) programming language.

## Features

- **Syntax Highlighting** - Full TextMate grammar for NAAb
- **Autocomplete** - Intelligent code completion for keywords, functions, variables, types
- **Hover Information** - Type information and function signatures on hover
- **Go to Definition** - Jump to function and variable definitions
- **Document Symbols** - Outline view with all functions, structs, enums
- **Real-time Diagnostics** - Parse and type errors as you type
- **Debounced Updates** - Optimized performance during rapid typing

## Requirements

- NAAb Language Server (`naab-lsp`) must be installed and in PATH
- Alternatively, set `naab.lsp.serverPath` in settings to point to the executable

## Installation

### From VSIX

```bash
code --install-extension naab-0.1.0.vsix
```

### From Source

```bash
cd vscode-naab
npm install
npm run compile
```

## Configuration

### Settings

- `naab.lsp.serverPath` - Path to naab-lsp executable (default: "naab-lsp")
- `naab.trace.server` - Trace LSP communication (default: "off")

### Example settings.json

```json
{
  "naab.lsp.serverPath": "/usr/local/bin/naab-lsp",
  "naab.trace.server": "verbose"
}
```

## Usage

1. Open a `.naab` file
2. The extension will automatically activate
3. Language server features will be available:
   - Type to get autocomplete (Ctrl+Space)
   - Hover over symbols to see type info
   - Ctrl+Click (or F12) to go to definition
   - Ctrl+Shift+O to see document outline

## Troubleshooting

### LSP Server Not Starting

1. Verify `naab-lsp` is installed:
   ```bash
   which naab-lsp
   naab-lsp --version
   ```

2. Check VS Code Output panel:
   - View → Output
   - Select "NAAb Language Server" from dropdown

3. Enable verbose logging:
   ```json
   {
     "naab.trace.server": "verbose"
   }
   ```

### No Autocomplete

1. Ensure file is saved with `.naab` extension
2. Check for syntax errors (red squiggles)
3. Restart VS Code

## Development

### Building from Source

```bash
# Install dependencies
npm install

# Compile TypeScript
npm run compile

# Watch mode
npm run watch

# Package extension
npx vsce package
```

### Testing

1. Open vscode-naab in VS Code
2. Press F5 to launch Extension Development Host
3. Open a `.naab` file in the development instance
4. Test features

## Known Limitations

- Multi-file go-to-definition not yet supported
- Member access completions (struct fields) incomplete
- No find references yet
- No rename refactoring yet

## Release Notes

### 0.1.0

Initial release of NAAb language support:
- Syntax highlighting
- LSP integration (completion, hover, definition, symbols, diagnostics)
- Performance optimizations (debouncing, caching)

## Contributing

Report issues and contribute at: https://github.com/b-macker/NAAb

## License

MIT License - See LICENSE file for details
