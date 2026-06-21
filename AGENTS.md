# AGENTS.md

This file defines repository-specific instructions for coding agents working in this tree.

## Core principle

Prefer explicit code at the call site over helper indirection when the code controls behavior, ownership, or synchronization.

Code should be documented through Doxygen docstrings. In particular, the design choices need to be explained. Add in comments explainations on what we are looping on, looking for what.

Design decisions are documented in ./doc Markdown files. While these might be slightly outdated compared to current code, regarding implementation, they hold the initial intents that should prevail in case of misunderstanding of the codebase or disagreements between several APIs.

## Helper policy

Do not introduce or keep helper functions whose main effect is to hide:

- fallback selection,
- lock acquire/release,
- ownership transfer,
- buffer/source selection,
- cleanup ordering,
- early returns or failure routing.

In these cases, prefer local duplication over abstraction. Never wrap API functions into functions that contain nothing else. 

Helpers are acceptable when they provide real value, for example:

- reusable math or geometry transforms,
- format conversion,
- data structure utilities,
- self-contained algorithms with no hidden resource lifetime,
- heavily-reused code blocks,
- complex branching that would make the control flow unlegible.

## Locking and lifetime

Keep lock lifetime visible in the caller whenever possible.

- Acquire locks in the same function that starts using the protected resource.
- Release locks in the same control path, or in one clearly paired teardown path.
- Do not return from helpers with locks still held unless explicitly requested by the user.
- Do not hide read/write lock transitions behind convenience wrappers if that obscures who owns the lock.
- Prefer code that passes static thread-safety analysis without suppression.
- Do not use thread-safety-analysis suppression macros to justify a design that can be expressed more explicitly.

## Error handling and contracts

Assume upstream contracts are valid unless the user asks for defensive programming.

- Do not add repeated checks for arguments and state already guaranteed by the caller.
- Keep checks only for failures owned by the local subsystem, such as allocation failure, cache miss, OpenCL failure, or unavailable GUI-only state.
- Collapse equivalent fallback paths into one visible path near the top of the function when practical.
- Functions and methods should return `int` values (or enum types if needed) that signal errors or success, their callers need to catch these errors and hoist them up the calling tree. We don't carry on with errors, we interrupt the control flow ASAP.
- NULL checks need to be performed at the highest-level in the calling tree, and downstream functions need to be prevented from even running if one of their critical input arguments is NULL. Don't NULL-check every pointer in every helper, that may hide harmful programer errors that need to be fixed higher in the tree.

## Editing style

- Prefer editing existing code over adding new abstraction layers.
- Prefer fewer symbols and fewer cross-file helper entry points.
- If a helper becomes a one-liner or only guards against `NULL`, inline it at call sites.
- Comments should explain non-obvious intent, not restate the code.
- Document public API with Doxygen docstrings.

## Validation

After non-trivial code changes, run the narrowest relevant build or test target that exercises the edited code.

## Coding style

- Functions that have many input arguments should take structures as input.
- Comparing large number of variables for equality should be achieved through `memcmp` over data structures when possible.
- All loops should be parallelized using OpenMP 4.5 when loop iterations write on different addresses and the overhead is worse it. Avoid false sharing on variables. Mind the fact that we also maintain builds without OpenMP.
- Pixels loops on RGBA should use `dt_aligned_pixel_simd_t` vector type when possible.
- Prefer linear branching when possible, over deeply nested cumulative conditions.
- Functions should contain at most 2 to 3 `if` as much as possible.
- Nesting `if` and `switch` on more than 3 levels is forbidden. Find a way to refactor your code.
- Create the least amount of data structures.
- Write the least amount of code.
- Reuse existing API as much as possible. Extend them if needed. Avoid code duplication.
- The maximum cyclomatic complexity of a single sourcecode file is 500. If you reach more than that, find a way to topically split features into subfiles.
- Use the least amount of public API, and keep as much as possible private.
- Write C in an object-oriented mindset: `.h` are public API, `.c` are private, both should inherit from parent but not from children, they should depend on the least amount of external resources and be fully enclosed (modular). Data structures are classes, they take properties and can take callbacks as methods. Implement abstract API using preprocessor macros.
- Never reference unused function input arguments as `(void)arg` inside functions. That only creates noise. API functions may have unused arguments, that's life.
- SQL queries should be hidden behind C APIs to be reused in the C code, don't put SQL code into GUI code, or modules.
- The code should be modular: features are split into self-enclosed modules that communicate with the core through interfaces. They should be unaware of other modules and unaware of the core, they should care about a minimal number of inputs and states to produce their output. The core is orchestrating communication between modules. Modules should not communicate with each other.

## Cross-platform printf format specifiers

We build on Linux, macOS and Windows, where `size_t`, `uint64_t` and friends have different underlying types (e.g. `size_t` is `unsigned long` on 64-bit macOS but `unsigned long long` on Windows). A hardcoded length modifier that compiles on Linux will break the macOS/Windows CI with `-Werror,-Wformat`. Always match the format to the argument's exact type, using portable macros instead of guessing `%lu`/`%llu`/`%zu`:

- `size_t` / `gsize` → `"%" G_GSIZE_FORMAT`
- `ssize_t` / `gssize` → `"%" G_GSSIZE_FORMAT`
- `uint64_t` → `"%" PRIu64`; `int64_t` → `"%" PRId64` (from `<inttypes.h>`)
- `uint32_t`/`int32_t` → `"%" PRIu32`/`"%" PRId32`
- `goffset` → `"%" G_GOFFSET_FORMAT`

Do not paper over a warning by casting the argument to whatever type the existing specifier expects; fix the specifier to match the real type. Never use bare `%zu`/`%llu` for portable code — MSVC runtimes do not reliably support them.

## Performance and optimization

- When optimizing code for performance, build the optimized assembly code, for example with `gcc -O3 -g -S -fverbose-asm file.c`, and analyze the assembly structure. Generic optimizations for package builds and local native optimizations should both be evaluated.