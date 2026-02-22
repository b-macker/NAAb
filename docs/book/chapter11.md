# Chapter 11: Composing Pipelines

The true power of NAAb lies in its ability to compose blocks from different languages into a coherent workflow. This "block assembly" process allows you to build complex applications by chaining together specialized components.

## 11.1 Importing and Aliasing Blocks

To use a block in your program, you must first import it using the `use` statement. It is best practice to give each block a meaningful alias that describes its function within your specific context.

```naab
// Import blocks with clear aliases
use BLOCK-PY-10234 as load_dataset
use BLOCK-JS-05678 as clean_data
use BLOCK-CPP-09912 as compute_statistics
```

## 11.2 Building a Data Pipeline

A typical data pipeline involves loading data, processing it, and then analyzing or saving the results. In NAAb, you can pass data between blocks of different languages transparently.

### 11.2.1 Sequential Composition

The most robust way to build a pipeline is by storing the result of each step in a variable and passing it to the next step. This is clear, easy to debug, and ensures type safety at each stage.

```naab
main {
    // Step 1: Load Data (Python)
    print("Step 1: Loading data...")
    let raw_data = load_dataset("data.csv")
    print("Data loaded, rows:", array.length(raw_data))

    // Step 2: Clean Data (JavaScript)
    print("Step 2: Cleaning data...")
    let cleaned_data = clean_data(raw_data)
    
    // Step 3: Compute Statistics (C++)
    print("Step 3: Analyzing...")
    let stats = compute_statistics(cleaned_data)

    print("Final Analysis:", stats)
}
```

### 11.2.2 The Pipeline Operator

As introduced in Chapter 4, NAAb includes a pipeline operator (`|>`) syntax for more fluid composition. This operator allows you to chain function calls across multiple lines, improving readability.

```naab
// Pipeline Syntax
let stats = "data.csv" 
    |> load_dataset 
    |> clean_data 
    |> compute_statistics
```

## 11.3 Validating Compatibility

Before running a pipeline, you might want to ensure that the blocks are compatible (e.g., that the output of `load_dataset` matches the input of `clean_data`).

The NAAb CLI provides a `validate` command for this purpose:

```bash
naab-lang validate "BLOCK-PY-10234,BLOCK-JS-05678,BLOCK-CPP-09912"
```

This command checks the type signatures of the blocks and reports any mismatches or suggests necessary adapters.

## 11.4 Pipeline Best Practices

1.  **Explicit Aliases**: Always alias imported blocks to readable names (`clean_data` vs `BLOCK-JS-05678`).
2.  **Type Awareness**: Be mindful of data types flowing between languages. NAAb handles serialization, but complex objects (like custom classes) are often best passed as JSON strings or dictionaries.
3.  **Modular testing**: Test each block individually (using `<<lang` snippets or isolated `use` calls) before assembling a long pipeline.
4.  **Error Handling**: Wrap pipeline execution in `try/catch` blocks to handle failures in foreign code gracefully.
