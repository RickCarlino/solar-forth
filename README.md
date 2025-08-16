# Solar Forth — Forth with libuv timers and TCP

Overview
- Goal: tiny and Forth-inspired with non-blocking timers and sockets using libuv.
- Non-blocking: timers and TCP are driven by `uv:run` (libuv’s event loop).

# Why

Most Forth systems I have seen don't have a good concurrency story, which makes it hard to write serious applications.

This repository explores the idea of building a Forth system that is tightly integrated with LibUV.

It is not an ANS Forth. 

Build
- Prereq: libuv development headers installed (e.g., `libuv-dev`).
- Build: `make`
- Run REPL: `./solarforth`
- Run script: `./solarforth examples/timer.frt`

# Syntax & Types

Run: `./solarforth examples/basics.frt`

```
: greet "Hello from definition" print cr ;
greet

\ one-shot timers using numbers and quotations
uv:timer 500 0 [ drop "first tick" print cr ] uv:timer-start
uv:timer 1000 0 [ drop "done" print cr bye ]  uv:timer-start
uv:run
```

- Tokens: space-separated. Comments use `\` to end-of-line or `( ... )` blocks.
- Numbers: 64-bit signed; parsed with base 0 (supports `123`, `0xFF`, `010`).
- Strings: double-quoted with escapes (`\n`, `\r`, `\t`, `\"`, `\\`).
- Quotations: `[ ... ]` pushes a quote (deferred code) onto the stack; nestable.
- Definitions: `: name ... ;` defines a new word. Quotes inside definitions are captured and inlined as literals.
- Stack machine: words consume/produce values. Types: `int`, `string`, `quote`, and `handle` (`timer` or `tcp`).

# Built-in Words

## Core

- `dup` (x -- x x): duplicate top of stack.
- `drop` (x --): drop top value (frees strings).
- `print` (str --): write string to stdout.
- `cr` ( -- ): newline.
- `words` ( -- ): list defined words.
- `bye` ( -- ): exit REPL.

## LibUV

- `uv:run` ( -- ): run event loop; processes timers and I/O.
- `uv:timer` ( -- h): create timer handle.
- `uv:timer-start` (h timeout-ms repeat-ms q --): start timer; runs `q` with `h` each tick.
- `uv:timer-stop` (h --): stop timer.
- `uv:close` (h --): close handle (timer or tcp); frees after close completes.
- `uv:tcp` ( -- h): create TCP handle.
- `uv:tcp-bind` (h ip port --): bind server (e.g., `h "0.0.0.0" 7000 uv:tcp-bind`).
- `uv:listen` (h backlog q --): listen; on accept invokes `q` with new client handle.
- `uv:read-start` (h q --): start reading; on data calls `q` with `h str`; on EOF calls with `h ""`.
- `uv:tcp-connect` (h ip port q --): connect; on success calls `q` with `h`.
- `uv:write` (h str --): write string to stream.

# Examples

One-shot timer: `./solarforth examples/timer.frt`
Echo server (127.0.0.1:7000): `./solarforth examples/echo_server.frt`
