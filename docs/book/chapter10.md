# Chapter 10: The Block Registry

One of NAAb's core strengths is its ecosystem of reusable code blocks. The Block Assembly System allows you to discover, inspect, and integrate pre-built functionality from a vast library, saving you from reinventing the wheel. This chapter explains how to navigate the Block Registry using the NAAb command-line tools.

## 10.1 Understanding Block Assembly

In NAAb, a "block" is a self-contained unit of code—often written in a foreign language like Python or C++—that performs a specific task. Blocks are defined by metadata that describes their inputs, outputs, and dependencies.

The Block Registry acts as a centralized database of these blocks. Instead of manually writing `<<python ... >>` snippets for every task, you can search the registry for a block that already does what you need (e.g., "validate email", "parse CSV", "train model") and `use` it directly in your code.

## 10.2 Searching for Blocks

The primary tool for interacting with the registry is the `naab-lang` CLI. Before using it, ensure you have indexed the block library (see Chapter 1).

### 10.2.1 Listing All Blocks

To see an overview of available blocks and their language distribution, use the `list` command:

```bash
naab-lang blocks list
```

This will display statistics about the total number of blocks and a breakdown by language (Python, C++, JavaScript, etc.).

### 10.2.2 Searching by Keyword

To find blocks relevant to a specific task, use the `search` command followed by your query:

```bash
naab-lang blocks search "validate email"
```

The output will list matching blocks, ordered by relevance.

You can filter results by language using the `--language` flag (if supported by the current CLI version).

## 10.3 Anatomy of a Block

Each block has a unique ID (e.g., `BLOCK-PY-09145`) and metadata that defines how to use it.

### 10.3.1 Using a Block in Code

Once you've found a block ID, you import it using the `use ... as ...` syntax:

```naab
use BLOCK-PY-09145 as validate_email

main {
    let result = validate_email("user@example.com")
    print("Is valid:", result)
}
```

### 10.3.2 Inspecting a Block

To see details about a specific block without importing it, use the `info` command:

```bash
naab-lang blocks info BLOCK-PY-09145
```

This will display the block's description, inputs, outputs, language, and other metadata.

This simple mechanism abstracts away the complexity of the underlying implementation language, allowing you to compose diverse functionalities as if they were native NAAb functions.