# Development Setup Guide

This document describes the VS Code configuration for consistent code formatting and linting across the project.

## Overview

The project uses:
- **clang-format** for automatic code formatting
- **clang-tidy** for static analysis and linting
- **C/C++ Extension** for IntelliSense and integration with the above tools

## Files

- `.clang-format` — Code formatting rules
- `.clang-tidy` — Static analysis configuration
- `.vscode/settings.json` — VS Code extension settings

## Installation

### 1. VS Code Extensions

Install the **C/C++** extension from Microsoft:
- Open VS Code Extensions (Cmd/Ctrl+Shift+X)
- Search for "C/C++"
- Install the official Microsoft extension

### 2. System Dependencies

Ensure `clang-format` and `clang-tidy` are installed:

```bash
# macOS (Homebrew)
brew install llvm

# Linux (Ubuntu/Debian)
sudo apt-get install clang-format clang-tools clang-tidy
```

## Code Style

The project follows these conventions (automatically enforced by clang-format):

### Formatting Rules

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 100 characters (soft limit, enforced at 100)
- **Braces**: Linux style (opening brace on same line)
- **Pointer/Reference**: Aligned to the left (`int* ptr`, not `int *ptr`)
- **Include sorting**: Grouped by system includes first, then project includes
- **Spacing**: 2 spaces before trailing comments, no spaces in casts/parentheses

### Example

```cpp
// Good: matches project style
namespace mylib {

class MyClass {
public:
    MyClass();
    int calculate(const char* input) const;

private:
    int value_;  // member variable with trailing underscore
};

}  // namespace mylib
```

### Naming Conventions

- **Classes**: `CamelCase` (e.g., `MyClass`, `ConfigParser`)
- **Functions**: `snake_case` (e.g., `calculate_value`, `parse_config`)
- **Variables**: `snake_case` (e.g., `counter`, `is_enabled`)
- **Constants**: `UPPER_CASE` (e.g., `MAX_BUFFER_SIZE`)
- **Macros**: `UPPER_CASE` (e.g., `DEBUG_LOG`)

## Usage

### Auto-Format on Save

Format on save is automatically enabled in VS Code. When you save a file, clang-format will automatically reformat it to match project style.

### Manual Formatting

Format the current file:
- **macOS/Linux**: `Shift+Option+F`
- **Windows**: `Shift+Alt+F`

Or right-click and select "Format Document".

### Format a Selection

Select code and use the same keyboard shortcut to format only the selection.

### Lint Analysis

Clang-tidy runs automatically in the background (as configured in `.vscode/settings.json`). Issues appear as:
- **Red squiggles**: Errors
- **Yellow squiggles**: Warnings

Hover over issues to see the suggestion. Click the lightbulb icon to apply quick fixes.

## Static Analysis Checks

The project enables the following clang-tidy checks:

- **readability-*** — Code readability
- **modernize-*** — C++11/14/17 modernization suggestions
- **performance-*** — Performance improvements
- **bugprone-*** — Likely bugs
- **cert-*** — Secure coding standards

These checks help catch:
- Unused variables
- Missing `override` keywords on virtual methods
- Implicit conversions that may lose precision
- Raw pointers that could be smart pointers
- Missing `const` qualifiers
- Non-idiomatic C++ patterns

## Project-Specific Notes

This project is:
- **C++ Standard**: C++11 (older standard for compatibility with ARM toolchain)
- **Target**: ARM Cortex-A9 (Mazda CMU) cross-compilation
- **Use case**: LD_PRELOAD-able shared libraries

Formatting is relaxed for:
- Long configuration documentation comments
- Inline macros that need custom formatting
- Generated dbus-c++ code (in `reference/dbus/`)

## CI/CD Integration

Before committing, you can manually check formatting compliance:

```bash
# Check formatting (non-destructive)
clang-format -style=file --dry-run -Werror src/**/*.cpp

# Auto-fix all files
find . -name "*.cpp" -o -name "*.h" | xargs clang-format -i --style=file
```

Add these to CI/CD pipelines to ensure all commits match project style.

## Troubleshooting

### clang-format not found
Ensure it's installed and in your PATH. On macOS with Homebrew, it's installed as:
```bash
/usr/local/opt/llvm/bin/clang-format
```

### IntelliSense not working
- Verify `.vscode/c_cpp_properties.json` has the correct include paths
- Reload VS Code (Cmd/Ctrl+Shift+P → "Developer: Reload Window")
- Check the C++ extension logs

### Formatting disagrees with my preference
The configuration is intentionally standardized to match the existing codebase. Changes should be discussed with the team before modifying `.clang-format`.

## Resources

- [clang-format Documentation](https://clang.llvm.org/docs/ClangFormat/)
- [clang-tidy Checks](https://clang.llvm.org/extra/clang-tidy/checks/)
- [C++ Core Guidelines](https://github.com/isocpp/CppCoreGuidelines)
