# NAAb Language Documentation

All documentation lives in the **book/** directory as a comprehensive reference organized by topic.

## The NAAb Book

| Chapter | Topic |
|---------|-------|
| [Chapter 1](book/chapter01.md) | Introduction & Architecture |
| [Chapter 2](book/chapter02.md) | Variables, Types & Mutability |
| [Chapter 3](book/chapter03.md) | Control Flow |
| [Chapter 4](book/chapter04.md) | Functions & Modules |
| [Chapter 4a](book/chapter04a.md) | Polyglot Async Execution |
| [Chapter 5](book/chapter05.md) | The Polyglot Block System |
| [Chapter 6](book/chapter06.md) | Language-Specific Integrations |
| [Chapter 7](book/chapter07.md) | Data Structures & Algorithms |
| [Chapter 8](book/chapter08.md) | Strings, Text, and Math |
| [Chapter 9](book/chapter09.md) | System Interaction |
| [Chapter 10](book/chapter10.md) | The Block Registry |
| [Chapter 11](book/chapter11.md) | Composing Pipelines |
| [Chapter 12](book/chapter12.md) | Error Handling |
| [Chapter 13](book/chapter13.md) | Security and Isolation |
| [Chapter 14](book/chapter14.md) | Testing and Quality Assurance |
| [Chapter 15](book/chapter15.md) | Performance Optimization |
| [Chapter 16](book/chapter16.md) | Tooling and the Development Environment |
| [Chapter 17](book/chapter17.md) | Case Studies |
| [Chapter 18](book/chapter18.md) | Networking and Data Formats |
| [Chapter 19](book/chapter19.md) | Cryptography and Security Modules |
| [Chapter 20](book/chapter20.md) | Future Roadmap |

## Quick Start

See [book/QUICK_START.md](book/QUICK_START.md) for getting started.

## Other Files

- [CHANGELOG.md](CHANGELOG.md) - Release history
- [CONTRIBUTING.md](CONTRIBUTING.md) - Contributor guidelines

## Verification Tests

Every chapter has runnable `.naab` test files in `tests/chapter verification/` that verify the documented features work correctly. Run all tests with:

```bash
cd ~/.naab/language && bash run-all-tests.sh
```
