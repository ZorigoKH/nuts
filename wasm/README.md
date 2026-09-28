# nuts in WebAssembly

The C++ library compiled with [Emscripten](https://emscripten.org) into an ES module, `nuts.mjs`,
which loads `nuts.wasm` (the compiled code, with the push/fold equity matrix built in). It runs
in a web page, a web worker and Node.js. The bindings are in `bindings.cpp` (embind).

## Build and test

From the repository root, with Emscripten installed (`emcc` on the PATH):

```sh
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm        # -> build-wasm/nuts.mjs and build-wasm/nuts.wasm
node wasm/smoke.mjs build-wasm  # checks known answers; exits with 1 on a failure
```

The C++ tests and tools are not built for WebAssembly (they use threads).

## Use

```js
import createNuts from "./nuts.mjs";  // nuts.wasm must be next to it

const nuts = await createNuts();
nuts.evaluate("As Ks Qs Js Ts");               // 7462
nuts.category(7462);                           // "Straight Flush"
nuts.parse_range("TT+, AKs").combos.length;    // 34
nuts.equity(["AhAs", "KdKc"], "", 0, 0);       // exact: {exact: true, deals: 1712304, players: [...]}
nuts.equity(["QQ+", "random"], "", 100000, 1); // Monte Carlo: 100,000 trials, seed 1
nuts.pushfold(10);                             // {shove: 13 x 13, call: 13 x 13, ...}
nuts.count_all_seven_card_hands();             // {categories: {...}, elapsed_ms, ...}
```

| function | returns |
|---|---|
| `evaluate(cards)` | 1..7462 for 5 to 7 cards (higher is better), 0 for invalid input |
| `category(rank)` | `"High Card"` ... `"Straight Flush"`, `""` for an invalid rank |
| `parse_range(text)` | `{combos: ["AsKs", ...]}` (range weights are not returned) |
| `equity(hands, board, trials, seed)` | `{exact, deals, players: [{hand, equity, win, tie, std_error}]}` |
| `pushfold(stack_bb)` | `{stack_bb, iterations, exploitability, sb_ev, hands, shove, call}` |
| `count_all_seven_card_hands()` | `{hands, distinct_values, royal_flushes, elapsed_ms, categories}` |

- `equity`: `hands` is an array with one string per player, two specific cards (`"AsKd"` or
  `"As Kd"`) or a range (`"TT+, AKs"`). When every player has two specific cards the result is exact (every
  completion of the board) and `trials` and `seed` are ignored; otherwise it is a Monte Carlo
  estimate from `trials` random deals, the same for the same `seed`.
- `pushfold` solves the heads-up push/fold game for that stack (in big blinds, 1 to 100) with
  CFR+; the grids are laid out like `hands` (`hands[0][0]` is `"AA"`, `hands[0][1]` `"AKs"`,
  `hands[1][0]` `"AKo"`). Same meaning as the Python `nuts.pushfold`.
- `count_all_seven_card_hands` evaluates all 133,784,560 seven-card hands, so it takes seconds;
  in a page, call it from a web worker so the page stays responsive.

Errors: the build has C++ exceptions turned off (Emscripten's default), so functions do not
throw; `parse_range`, `equity` and `pushfold` return `{error: "message"}` instead of a result.

Threads: none. WebAssembly threads need `SharedArrayBuffer`, which a page only gets when its
server sends special headers, so the Monte Carlo code runs on one thread here.

## Size and speed

Built with Emscripten 6.0.10 (`-O3`, Release). Sizes:

| file | bytes | gzipped |
|---|---|---|
| `nuts.wasm` | 320,068 (228,504 of them the equity matrix) | 153,284 |
| `nuts.mjs` | 31,968 | 9,393 |

Timings in Node.js 26.10 on an Apple M3, median of 5 calls after 1 warm-up call, measured twice
while another heavy job was running (load average about 10 for the first run, 8 plus a
ThreadSanitizer test run for the second). For comparison, the native Release build
(`nuts_bench`, Apple clang 21, `-O3 -DNDEBUG`, same machine, load average about 9):

| | WebAssembly, run 1 | run 2 | native |
|---|---|---|---|
| `count_all_seven_card_hands()` (133,784,560 hands) | 1.43 s | 1.68 s | 1.20 s |
| exact equity, AhAs vs KdKc preflop (1,712,304 boards) | 40 ms | 46 ms | 31 ms |
| Monte Carlo, AA vs random, 1,000,000 trials, 1 thread | 42 ms (†) | | 38 ms (*) |
| `pushfold(10)`, later calls | 54-57 ms | 65-88 ms | |
| `pushfold(10)`, first call | 151 ms | 228 ms | |

(*) `nuts_bench` times 10,000,000 trials (382.0 ms, 26.2 M trials/s); scaled to 1,000,000.

(†) Measured again, WebAssembly and native, after a player with the range "random" started to
be dealt from the cards left over (load average about 4). Before that change: 44 and 52 ms
(WebAssembly), 43 ms (native).

The first `pushfold` call also reads the built-in matrix, and the first calls in general run
before the JavaScript engine has fully optimized the WebAssembly code.
