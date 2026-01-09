# NAAb Block Assembly Language - Formal Grammar

## Overview

The `.naab` language is the world's first **block assembly language** where programs are written by importing and composing reusable code blocks from any programming language.

## Lexical Structure

### Keywords
```
use as function async method return if else for in while
match try catch struct class init module export import
config main let const await new
```

### Operators
```
+ - * / %           # Arithmetic
== != < <= > >=     # Comparison
&& ||               # Logical (alternative: and, or)
!                   # Not
=                   # Assignment
|>                  # Pipeline
->                  # Function return type
=>                  # Lambda/match arrow
.                   # Member access
::                  # Scope resolution
```

### Delimiters
```
( ) { } [ ]         # Grouping
, ;                 # Separators
:                   # Type annotation
```

### Identifiers
```
identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
```

### Block IDs
```
block_id ::= BLOCK-[A-Z]+-[0-9]+
```

Examples: `BLOCK-CPP-03845`, `BLOCK-PY-00123`, `BLOCK-JS-00042`

### Literals
```
integer    ::= [0-9]+
float      ::= [0-9]+ '.' [0-9]+
string     ::= '"' [^"]* '"' | "'" [^']* "'"
boolean    ::= 'true' | 'false'
```

## Syntax Grammar (EBNF)

### Program Structure

```ebnf
program ::= import_list function_list main_block?

import_list ::= use_statement*

use_statement ::= 'use' block_id 'as' identifier

function_list ::= function_decl*

main_block ::= 'main' compound_stmt
```

### Declarations

```ebnf
function_decl ::= 'function' identifier '(' parameter_list? ')'
                  ('->' type)? compound_stmt

parameter_list ::= parameter (',' parameter)*

parameter ::= identifier ':' type ('=' expression)?

struct_decl ::= 'struct' identifier '{' struct_field* '}'

struct_field ::= identifier ':' type ';'
```

**Examples**:
```naab
struct Point {
    x: INT;
    y: INT;
}

struct Person {
    name: STRING;
    age: INT;
    email: STRING;
}
```

### Types

```ebnf
type ::= 'int' | 'float' | 'string' | 'bool' | 'void' | 'any'
       | 'dict' ('[' type ',' type ']')?
       | 'list' ('[' type ']')?
       | identifier  # Block type or user-defined type
```

### Statements

```ebnf
statement ::= compound_stmt
            | expr_stmt
            | return_stmt
            | if_stmt
            | for_stmt
            | while_stmt
            | var_decl_stmt

compound_stmt ::= '{' statement* '}'

expr_stmt ::= expression

return_stmt ::= 'return' expression?

if_stmt ::= 'if' '(' expression ')' statement ('else' statement)?

for_stmt ::= 'for' '(' identifier 'in' expression ')' statement

while_stmt ::= 'while' '(' expression ')' statement

var_decl_stmt ::= ('let' | 'const')? identifier ('=' expression)?
```

### Expressions

```ebnf
expression ::= assignment_expr

assignment_expr ::= pipeline_expr ('=' assignment_expr)?

pipeline_expr ::= logical_or_expr ('|>' logical_or_expr)*

logical_or_expr ::= logical_and_expr ('||' logical_and_expr)*

logical_and_expr ::= equality_expr ('&&' equality_expr)*

equality_expr ::= relational_expr (('==' | '!=') relational_expr)*

relational_expr ::= additive_expr (('<' | '<=' | '>' | '>=') additive_expr)*

additive_expr ::= multiplicative_expr (('+' | '-') multiplicative_expr)*

multiplicative_expr ::= unary_expr (('*' | '/' | '%') unary_expr)*

unary_expr ::= ('!' | '-' | '+') unary_expr
             | postfix_expr

postfix_expr ::= primary_expr postfix_op*

postfix_op ::= '(' argument_list? ')'    # Function call
             | '.' identifier             # Member access
             | '[' expression ']'         # Index access

primary_expr ::= identifier
               | literal
               | '(' expression ')'
               | dict_literal
               | list_literal
               | struct_literal

argument_list ::= expression (',' expression)*

struct_literal ::= 'new' identifier '{' struct_init_list? '}'

struct_init_list ::= struct_init (',' struct_init)*

struct_init ::= identifier ':' expression
```

**Struct Literal Examples**:
```naab
# Creating struct instances
let p = new Point { x: 10, y: 20 }
let person = new Person { name: "Alice", age: 30, email: "alice@example.com" }

# Nested structs
let line = new Line {
    start: new Point { x: 0, y: 0 },
    end: new Point { x: 100, y: 200 }
}

# Structs in arrays
let points = [
    new Point { x: 1, y: 2 },
    new Point { x: 3, y: 4 }
]
```

```ebnf
dict_literal ::= '{' dict_entry_list? '}'

dict_entry_list ::= dict_entry (',' dict_entry)*

dict_entry ::= expression ':' expression

list_literal ::= '[' expression_list? ']'

expression_list ::= expression (',' expression)*

literal ::= integer | float | string | boolean
```

## Example Programs

### Hello World with C++ Block

```naab
# Import Abseil Cord block from C++
use BLOCK-CPP-03845 as Cord

function process_data(input: string) -> dict {
    # Use C++ block directly (native call!)
    cord = Cord.from_string(input)
    normalized = cord.normalize()

    return {
        "data": normalized.to_string(),
        "length": normalized.length()
    }
}

main {
    result = process_data("Hello from .naab language!")
    print(result)
}
```

### Cross-Language Block Assembly

```naab
# Mix blocks from different languages
use BLOCK-CPP-03845 as Cord      # C++ string handling
use BLOCK-PY-00123 as FastAPI    # Python web framework
use BLOCK-JS-00042 as React      # JavaScript UI

function build_app() {
    # C++ for string processing
    data = Cord.from_string("Fast data")

    # Python for backend
    api = FastAPI.create_app()
    api.add_route("/data", lambda: data.to_string())

    # JavaScript for frontend
    ui = React.create_component({
        fetch_data: api.get_endpoint("/data")
    })

    return {api: api, ui: ui}
}

main {
    app = build_app()
    print("App ready!")
}
```

### Pipeline Operator

```naab
use BLOCK-PY-00234 as DataProcessor

function analyze(data: list[int]) -> dict {
    # Pipeline operator chains operations
    result = data
        |> DataProcessor.filter(x => x > 0)
        |> DataProcessor.map(x => x * 2)
        |> DataProcessor.sum()

    return {"result": result}
}
```

## Operator Precedence (highest to lowest)

1. `()` `[]` `.` - Grouping, indexing, member access
2. `!` `-` `+` (unary) - Unary operators
3. `*` `/` `%` - Multiplication, division, modulo
4. `+` `-` - Addition, subtraction
5. `<` `<=` `>` `>=` - Relational
6. `==` `!=` - Equality
7. `&&` - Logical AND
8. `||` - Logical OR
9. `|>` - Pipeline
10. `=` - Assignment

## Type System

### Built-in Types

- `int` - 64-bit signed integer
- `float` - 64-bit floating point
- `string` - UTF-8 string
- `bool` - Boolean (true/false)
- `void` - No return value
- `any` - Dynamic type (accepts anything)
- `dict[K, V]` - Dictionary/map
- `list[T]` - List/array

### Block Types

Blocks imported via `use` statements have their own types based on their source language:

- C++ blocks: Native C++ types (std::string → string, int → int, etc.)
- Python blocks: Python types mapped to .naab types
- JavaScript blocks: JS types mapped to .naab types

Type mapper automatically converts between languages.

## Semantic Rules

1. **Block imports must appear before functions**: All `use` statements must come before function definitions
2. **Main block is optional**: Programs without `main` are libraries
3. **Strong typing with inference**: Variables have types, inferred from initialization
4. **Block safety**: Block IDs verified at compile time against registry
5. **Cross-language safety**: Type conversions checked via type mapper

## Comments

```naab
# Single-line comment

# Multi-line comments use multiple single-line comments
# like this
```

## Reserved for Future

```
async await      # Async/await primitives
match case       # Pattern matching
try catch        # Exception handling
struct class     # User-defined types
module export    # Module system
```

## Parsing Strategy

The parser uses **recursive descent** with operator precedence parsing for expressions, adapted from LLVM/Clang parser blocks (BLOCK-CPP-07000+).

### Key Parser Functions

- `parseProgram()` - Entry point
- `parseUseStatement()` - Import blocks
- `parseFunctionDecl()` - Function definitions
- `parseStatement()` - Statements
- `parseExpression()` - Expressions with precedence
- `parsePrimaryExpr()` - Literals, identifiers, grouping

## Error Recovery

Parser includes error recovery strategies:

1. **Synchronization points**: After `;`, `}`, newlines
2. **Panic mode**: Skip to next statement on error
3. **Error production rules**: Handle common mistakes
4. **Clear error messages**: Include line/column and suggestions

---

**Status:** Grammar specification complete ✅
**Date:** 2025-12-16
**Version:** 0.1.0
