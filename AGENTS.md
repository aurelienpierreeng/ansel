# AGENTS.md

This file defines repository-specific instructions for coding agents working in this tree.

## Core principle

Prefer explicit code at the call site over helper indirection when the code controls behavior, ownership, or synchronization.

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
- Functions and methods should return `int` values (or enum types if needed) that signal errors or success, their callers need to catch these error and hoist them up the calling tree. We don't carry on with errors, we interrupt the control flow ASAP.
- NULL checks need to be performed at the highest-level in the calling tree, and downstream functions need to be prevented from even running if one of their critical input arguments is NULL. Don't NULL-check every pointer in every helper.

## Editing style

- Prefer editing existing code over adding new abstraction layers.
- Prefer fewer symbols and fewer cross-file helper entry points.
- If a helper becomes a one-liner or only guards against `NULL`, inline it at call sites.
- Comments should explain non-obvious intent, not restate the code.

## Validation

After non-trivial code changes, run the narrowest relevant build or test target that exercises the edited code.
