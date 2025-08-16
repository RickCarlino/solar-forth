# Repository Guidelines

## Project Structure & Module Organization
- `src/solarforth.c`: single-file C11 VM and libuv bindings (core runtime and words).
- `examples/*.frt`: runnable Forth scripts (e.g., `timer.frt`, `echo_server.frt`).
- `Makefile`: build targets and flags; outputs `solarforth`.
- Generated binary: `./solarforth` (REPL and script runner).

## Build, Test, and Development Commands
- Build: `make` — compiles `src/solarforth.c` with `-O2 -Wall -Wextra -std=c11` and links `-luv`.
- Clean: `make clean` — removes the `solarforth` binary.
- Run REPL: `./solarforth` — interactive prompt; try `words` then `cr`.
- Run script: `./solarforth examples/timer.frt` or `examples/echo_server.frt`.
- Debug build (optional): `make CFLAGS='-g -O0 -Wall -Wextra -std=c11'` (add `-fsanitize=address` if available).

## Coding Style & Naming Conventions
- Language: C11; match the style in `src/solarforth.c`.
- Indentation: spaces, consistent with existing file; no tabs.
- Identifiers: snake_case for functions (`prim_uv_timer_start`), PascalCase for structs (`Context`, `Handle`), ALL_CAPS for enums/constants.
- Scope: prefer `static` for internal functions; keep the VM surface minimal.
- Forth words: LibUV vocabulary uses `uv:*` prefix for libuv operations (e.g., `uv:timer-start`).

## Testing Guidelines
- Framework: none yet. Use example scripts and the REPL for regression checks.
- Add cases under `examples/` as small `.frt` programs demonstrating behavior.
- Quick checks: run `words`, timers (`uv:timer ... uv:timer-start uv:run`), and TCP echo with `nc 127.0.0.1 7000` against `examples/echo_server.frt`.
- Reliability: avoid blocking calls; ensure handles are closed via `uv:close` to prevent leaks.

## Commit & Pull Request Guidelines
- Commits: concise, imperative subject (≤72 chars), optional body with rationale and user impact. Reference issues when applicable.
- PRs: include a clear summary, what changed, how to test (exact commands), and sample REPL/script output. Mention environment needs (libuv dev headers).
- Scope: small, focused PRs; keep changes localized and consistent with existing patterns.

## Security & Configuration Tips
- Dependency: requires libuv development headers (e.g., `libuv-dev`).
- Networking: examples bind to high ports (e.g., `7000`); avoid privileged ports. Validate and sanitize strings passed from the Forth layer if adding new words.
