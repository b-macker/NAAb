# AEGIS-CLI: Automated Engineering Guidance & Intelligence System
## The Polyglot Terminal Assistant

**Status**: DRAFT PLAN
**Author**: Gemini (via NAAb Agent)
**Date**: February 18, 2026
**Version**: 1.0.0

---

## 1. Project Overview
**Aegis-CLI** is a context-aware, command-line AI assistant designed to help developers troubleshoot errors, explain code, and execute complex workflows directly from their terminal. Unlike simulations, **Aegis connects to real LLM APIs** (OpenAI, Anthropic, or local Ollama) and interacts with the **live filesystem and OS**.

It leverages the **NAAb language's unique polyglot capabilities** to distribute responsibilities across languages best suited for them:
- **NAAb**: Orchestration, configuration management, state handling.
- **Python**: LLM interaction, token counting, complex JSON processing.
- **Rust**: High-performance file indexing, hashing, and "ignoring" logic (.gitignore parsing).
- **Bash**: System command execution and environment probing.
- **JavaScript/Node**: Rich terminal UI rendering (spinners, colors, markdown tables).

---

## 2. Core Architecture

### The "Brain" (Python & NAAb)
- **NAAb Main Loop**: The REPL interface that accepts user input.
- **Context Window Manager (Python)**: Intelligently selects relevant file snippets to include in the LLM prompt based on relevance scores.
- **LLM Client (Python)**: Handles API requests to OpenAI/Anthropic/Ollama using standard libraries (no external pip dependencies required).

### The "Eyes" (Rust)
- **File Scanner**: A Rust block that rapidly traverses directories, respecting `.gitignore` rules.
- **Content Hasher**: Calculates SHA-256 hashes of files to detect changes and avoid re-indexing unchanged files.

### The "Hands" (Bash & NAAb)
- **Command Executor**: Safely runs shell commands suggested by the AI (with user confirmation).
- **System Probe**: Gathers OS info, environment variables, and tool versions to give the AI context.

### The "Face" (JavaScript/Node)
- **UI Renderer**: Takes raw text/JSON from other modules and renders beautiful ANSI-colored output, progress bars, and markdown tables.

---

## 3. Detailed Module Breakdown

### `main.naab` (Entry Point)
- **Role**: Application lifecycle management.
- **Responsibilities**:
  - Loads configuration (`config.json`).
  - Initializes the local database (JSON file).
  - Starts the REPL loop.
  - Routes commands (e.g., `/scan`, `/ask`, `/exec`).

### `modules/intelligence.naab` (Python Block)
- **Role**: AI Logic & API Interface.
- **Polyglot Implementation**:
  - Uses `<<python ... >>` blocks.
  - **Inputs**: User query, file context (JSON string), conversation history.
  - **Outputs**: Structured JSON response containing the answer, suggested commands, and updated history.
  - **Key Feature**: Implements a sliding window context manager to keep prompts within token limits.

### `modules/scanner.naab` (Rust Block)
- **Role**: High-Performance File System Operations.
- **Polyglot Implementation**:
  - Uses `<<rust ... >>` blocks.
  - **Function**: `scan_workspace(root_path, ignore_patterns) -> json_file_tree`.
  - **Why Rust?**: File walking and hashing can be slow in interpreted languages. Rust ensures the "scan" command is instant even for large repos.

### `modules/executor.naab` (Bash Block)
- **Role**: System Interaction.
- **Polyglot Implementation**:
  - Uses `<<bash ... >>` blocks.
  - **Safety**: Wraps commands in a "dry run" check or requires explicit user confirmation flags.
  - **Capture**: Captures `stdout` and `stderr` separately to feed back into the AI context for error analysis.

### `modules/ui.naab` (JavaScript Block)
- **Role**: Terminal formatting.
- **Polyglot Implementation**:
  - Uses `<<javascript ... >>` blocks.
  - **Function**: `render_markdown(text)`, `show_spinner(msg)`, `print_table(data)`.
  - **Why JS?**: Excellent string manipulation and widely available logic for ANSI escape codes.

---

## 4. "Customer Ready" Features

### 1. Robust Configuration
- **`~/.aegis/config.json`**: Stores API keys (encrypted or env var references), preferred model (GPT-4o, Claude 3.5, Llama 3), and custom ignore patterns.
- **Setup Wizard**: First run detects missing config and guides the user through setup.

### 2. Safety & Permissions
- **"Human-in-the-loop"**: Any command that modifies files or runs shell commands requires a `[y/N]` confirmation prompt.
- **Audit Logging**: All AI actions and executed commands are logged to `~/.aegis/audit.log` for transparency.

### 3. State Persistence
- **Conversation History**: Chats are saved to `.aegis/history.json` so context is preserved between sessions.
- **Vector Index (Lite)**: File hashes and simple keyword indices are saved to disk to avoid re-scanning the whole project every time.

---

## 5. Implementation Roadmap

### Phase 1: Foundation
1.  **Scaffold Project**: Create directory structure and `main.naab`.
2.  **Config Loader**: Implement `config.naab` to read/write JSON settings.
3.  **Basic REPL**: Implement the main input loop.

### Phase 2: The "Eyes" (Rust)
1.  **Scanner Module**: Implement `scanner.naab` with Rust blocks to list files and compute hashes.
2.  **Ignore Logic**: Add basic `.gitignore` parsing in Rust.

### Phase 3: The "Brain" (Python)
1.  **API Client**: Implement `intelligence.naab` using Python `http.client` (standard lib) to call OpenAI/Anthropic APIs.
2.  **Prompt Engineering**: Design the system prompt to include file context and respond in JSON.

### Phase 4: The "Face" & "Hands"
1.  **UI Module**: Add `ui.naab` for pretty printing.
2.  **Executor**: Add `executor.naab` for running shell commands.

### Phase 5: Integration & Polish
1.  **Connect Components**: Wire up the REPL to the scanner and intelligence modules.
2.  **Error Handling**: Add try/catch blocks and user-friendly error messages.
3.  **Testing**: Verify with a real codebase.

---

## 6. Success Criteria
- [ ] Application launches and prompts for config on first run.
- [ ] Scanning a medium-sized project (<1000 files) takes <1 second (thanks to Rust).
- [ ] User can ask "What does main.naab do?" and get a coherent answer using the LLM.
- [ ] User can ask "Run the tests" and Aegis suggests the correct command based on file analysis (e.g., detecting `package.json` vs `Cargo.toml`).
- [ ] All code is contained within the project directory; no global dependencies (pip/npm/cargo) required beyond the standard runtimes.
