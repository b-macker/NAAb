# naab-sys

Rust FFI bindings for NAAb Block Assembly Language.

## Overview

`naab-sys` provides safe, idiomatic Rust wrappers around the C FFI interface for creating NAAb blocks. Write high-performance blocks in Rust with minimal boilerplate.

## Features

- **Safe FFI Wrappers**: Automatic memory management and type safety
- **Ergonomic API**: Idiomatic Rust value types
- **Convenient Macros**: `naab_block!` macro for easy block creation
- **Type Conversions**: Seamless conversion between Rust and NAAb types
- **Zero-copy where possible**: Efficient data handling

## Quick Start

### Installation

Add to your `Cargo.toml`:

```toml
[dependencies]
naab-sys = { path = "../path/to/naab-sys" }

[lib]
crate-type = ["cdylib"]
```

### Example Block

```rust
use naab_sys::prelude::*;

naab_block!(add_numbers, |args: Vec<Value>| {
    let a = get_int_arg!(args, 0, 0);
    let b = get_int_arg!(args, 1, 0);
    Value::Int(a + b)
});
```

### Build

```bash
cargo build --release
```

### Use from NAAb

```naab
let result = call("rust://./target/release/libmy_block.so::add_numbers", 5, 10)
print(result)  // 15
```

## API Documentation

### Value Types

The `Value` enum represents all NAAb value types:

```rust
pub enum Value {
    Int(i32),
    Double(f64),
    Bool(bool),
    String(String),
    Void,
}
```

### Macros

#### `naab_block!`

Creates an extern "C" function that can be called from NAAb:

```rust
naab_block!(my_function, |args: Vec<Value>| {
    // Your code here
    Value::Int(42)
});
```

#### Helper Macros

- `get_int_arg!(args, index, default)` - Extract integer argument
- `get_double_arg!(args, index, default)` - Extract double argument
- `get_bool_arg!(args, index, default)` - Extract boolean argument
- `get_string_arg!(args, index, default)` - Extract string argument

## Examples

See the `examples/` directory for more examples:

- `add_one.rs` - Simple arithmetic
- More examples coming soon...

## Building Examples

```bash
cargo build --example add_one --release
```

## Testing

```bash
cargo test
```

## Requirements

- Rust 2021 edition or later
- NAAb runtime with Rust FFI support (Phase 3.1)

## License

MIT

## Contributing

This crate is part of the NAAb project. See the main NAAb repository for contribution guidelines.
