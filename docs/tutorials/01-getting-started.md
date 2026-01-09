# Tutorial 1: Getting Started with NAAb

**Duration:** 10 minutes
**Prerequisites:** None

---

## What You'll Learn

- How to write your first NAAb program
- Basic syntax and structure
- Running NAAb programs
- Using the REPL

---

## Step 1: Your First Program

Create a file called `hello.naab`:

```naab
main {
    print("Hello, NAAb!")
}
```

Run it:

```bash
$ naab-lang run hello.naab
Hello, NAAb!
```

**Congratulations!** You just ran your first NAAb program! 🎉

---

## Step 2: Variables

Let's add some variables. Update `hello.naab`:

```naab
main {
    let name = "Alice"
    let age = 30

    print("Name: " + name)
    print(age)
}
```

Run:

```bash
$ naab-lang run hello.naab
Name: Alice
30
```

---

## Step 3: Functions

Add a function:

```naab
fn greet(name: string) -> string {
    return "Hello, " + name + "!"
}

main {
    let message = greet("Bob")
    print(message)

    print(greet("Charlie"))
}
```

Output:

```
Hello, Bob!
Hello, Charlie!
```

---

## Step 4: Using the Standard Library

Import and use the `string` module:

```naab
import string

fn shout(text: string) -> string {
    return string.upper(text)
}

main {
    let loud = shout("hello world")
    print(loud)
}
```

Output:

```
HELLO WORLD
```

---

## Step 5: Try the REPL

Launch the interactive REPL:

```bash
$ naab-repl
NAAb REPL v0.1.0
Type 'help' for help, 'exit' to quit

>>> let x = 10
>>> let y = 20
>>> x + y
30

>>> import string
>>> string.upper("hello")
"HELLO"

>>> exit
```

---

## Next Steps

- [Tutorial 2: Variables and Types](02-variables-types.md)
- [Tutorial 3: Functions](03-functions.md)
- [Tutorial 4: Working with Blocks](04-blocks.md)

---

**You're ready to continue learning NAAb!** 🚀
