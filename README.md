# nuts

[![CI](https://github.com/ZorigoKH/nuts/actions/workflows/ci.yml/badge.svg)](https://github.com/ZorigoKH/nuts/actions/workflows/ci.yml)

A poker engine with a C++20 core: a 7-card hand evaluator verified over all 133,784,560
seven-card hands, exact and Monte Carlo equity for hands and ranges, and CFR solvers for Kuhn
poker and heads-up push/fold, with Python and WebAssembly bindings.

## What it does

- **Hand evaluation.** `evaluate5/6/7` and `evaluate(mask)` map 5 to 7 cards to a `HandRank` in
  1..7462 (higher is better; 7462 is a royal flush), using two small lookup tables: a flush table
  and a perfect hash of rank multisets. About 125 million 7-card evaluations per second on one
  core of an Apple M3. A slow, obviously-correct reference evaluator is kept for testing.
- **Equity.** Exact equity for 2 to 23 players with known hole cards (every completion of the
  board), and Monte Carlo equity for ranges (`"TT+, AKs, KQo:0.5"`) with a standard error,
  multithreaded and reproducible: the same seed and thread count give bit-for-bit the same
  result.
- **Solvers.** Vanilla CFR for Kuhn poker (reaches the known game value of −1/18), and CFR+ for
  heads-up push/fold, solved from an exact 169 × 169 preflop equity matrix with card removal.
  Equilibria for 1 to 20 bb are in [`data/pushfold.json`](data/pushfold.json) and are compared
  with HoldemResources' published charts in
  [`docs/pushfold_comparison.md`](docs/pushfold_comparison.md).
- **Bindings.** A Python package (`nuts`, built with nanobind) and a WebAssembly ES module
  (`nuts.mjs` + `nuts.wasm`, built with Emscripten).

## Quick start

### C++

Needs CMake 3.24+, a C++20 compiler and Ninja (or drop `-G Ninja`). The tests download Catch2
v3.16.0 through CMake's FetchContent, pinned by its SHA-256.

```sh
git clone https://github.com/ZorigoKH/nuts && cd nuts
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # 67 test cases
./build/nuts_tests "[exhaustive]"            # all 133,784,560 seven-card hands, about 2 s
./build/nuts_bench                           # the benchmarks below
```

To use the library from another CMake project, add it as a subdirectory (tests and tools are
then off by default) and link `nuts::nuts`:

```cmake
add_subdirectory(nuts)
target_link_libraries(app PRIVATE nuts::nuts)
```

```cpp
#include <iostream>
#include <vector>

#include "nuts/equity.hpp"
#include "nuts/evaluator.hpp"
#include "nuts/range.hpp"

int main() {
  // A 7-card hand is worth its best five cards; higher HandRank is better (1..7462).
  const auto cards = nuts::parse_cards("Ah Kh 9h 7h 5h 3h 2c");
  const nuts::HandRank rank = nuts::evaluate(nuts::mask_of(*cards));
  std::cout << rank << ' ' << nuts::category_name(nuts::category(rank)) << '\n';  // 7025 Flush

  // Exact equity: every one of the C(48, 5) = 1,712,304 boards.
  const std::vector<nuts::HoleCards> hands = {*nuts::parse_hole_cards("AhAs"),
                                              *nuts::parse_hole_cards("KdKc")};
  const auto exact = nuts::equity_exact(hands, /*board=*/{});
  std::cout << exact->players[0].equity << '\n';  // 0.812555

  // Monte Carlo for ranges: 1,000,000 trials, seed 7, 8 threads -> 0.819604 +- 0.000383012.
  const std::vector<nuts::Range> ranges = {*nuts::parse_range("AA"), *nuts::parse_range("KK")};
  const auto mc = nuts::equity_monte_carlo(ranges, /*board=*/{}, 1'000'000, /*seed=*/7,
                                           /*threads=*/8);
  std::cout << mc->players[0].equity << " +- " << mc->players[0].std_error << '\n';
}
```

Functions that can fail on bad input return `std::optional` (and an optional error message);
the evaluator returns 0 for an invalid hand. Nothing throws on the hot paths.

### Python

```sh
pip install git+https://github.com/ZorigoKH/nuts
```

This compiles the C++ core (scikit-build-core + nanobind), so it needs a C++20 compiler.
Python 3.10 or newer. The distribution is named `nuts-poker` (`nuts` is taken on PyPI); the
import name is `nuts`.

```python
>>> import nuts
>>> nuts.evaluate("As Ks Qs Js Ts")
7462
>>> nuts.category(nuts.evaluate("Ah Kh 9h 7h 5h 3h 2c"))
'Flush'
>>> [round(p["equity"], 4) for p in nuts.equity(["AhAs", "KdKc"])]            # exact
[0.8126, 0.1874]
>>> [round(p["equity"], 4) for p in nuts.equity(["AsKs", "QhQd", "7c6c"], "Qs Jh 2c")]
[0.1584, 0.8106, 0.031]
>>> r = nuts.equity(["AA", "KK"], trials=1_000_000, seed=7, threads=8)[0]      # Monte Carlo
>>> round(r["equity"], 4), round(r["std_error"], 4)
(0.8196, 0.0004)
>>> len(nuts.parse_range("TT+, AKs, KQo"))
46
>>> nuts.kuhn_cfr(100_000)["game_value"]                                      # -1/18 = -0.0555556
-0.05555470109817983
>>> p = nuts.pushfold(10)             # 13 x 13 grids: p["hands"], p["shove"], p["call"]
>>> p["hands"][0][:3], p["iterations"], p["exploitability"] < 1e-6
(['AA', 'AKs', 'AQs'], 1450, True)
```

`equity` is exact when every player has two specific cards and Monte Carlo otherwise (force
either with `exact=`). The long calls release the GIL. To run the tests from a checkout:

```sh
pip install cython setuptools wheel && pip install --no-build-isolation eval7   # no 3.13 wheel
pip install ".[test]" && pytest tests/python   # 79 tests
```

### WebAssembly

```sh
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm          # build-wasm/nuts.mjs + build-wasm/nuts.wasm
node wasm/smoke.mjs build-wasm    # known answers; exit status 1 on a failure
```

```js
import createNuts from "./nuts.mjs";   // nuts.wasm must sit next to it
const nuts = await createNuts();
nuts.evaluate("As Ks Qs Js Ts");                 // 7462
nuts.equity(["AhAs", "KdKc"], "", 0, 0);         // exact (trials and seed are ignored)
nuts.equity(["QQ+", "random"], "", 100000, 1);   // Monte Carlo: 100,000 trials, seed 1
nuts.pushfold(10);                               // {shove, call, hands, exploitability, ...}
nuts.count_all_seven_card_hands();               // the exhaustive check, in the browser
```

The module is built for pages, web workers and Node.js (the smoke test runs it in Node). It is
single-threaded (see [Limitations](#limitations)) and has C++ exceptions off, so errors come back
as `{error: "..."}`. The full API is in [`wasm/README.md`](wasm/README.md).

## How the evaluator works

The code is in [`src/evaluator.cpp`](src/evaluator.cpp); the scheme is the one used by
HenryRLee's PokerHandEvaluator.

**Hand values.** Every 5-card hand falls into one of exactly 7,462 equivalence classes. A
`HandRank` numbers them 1..7462 in blocks by category (high card 1–1277, one pair 1278–4137, ...,
straight flush 7453–7462), so `category(rank)` is a short loop over the category boundaries and
comparing two hands is comparing two integers. A 6- or 7-card hand gets the rank of its best
five cards.

**Card encoding.** A card is `id = 4 * rank + suit` (rank 0..12 = 2..A, suit 0..3 = c, d, h, s),
and a hand is a 64-bit mask with bit `id` set for each card. The four suits of one rank are four
adjacent bits, which the two steps below both rely on.

**Step 1: flushes.** `(mask >> suit) & 0x0001111111111111` keeps one bit per rank for the cards
of that suit. If some suit has 5 or more cards, those bits, gathered into a 13-bit mask, index
an 8,192-entry `uint16` table holding the best flush (or straight flush) those ranks make, and
that is the answer. Among 7 cards only one suit can have 5, and no non-flush hand can beat that
flush: the only categories above a flush are full house, four of a kind and straight flush. The
suited cards all have different ranks, so a full house or quads needs repeated ranks from the at
most 2 other cards, and it needs 3 of them (two more cards for the three of a kind plus one for
the pair, or three more for the quads). A straight flush lies entirely in the suit, so the flush
table already has it.

**Step 2: everything else.** Without a flush, suits no longer matter; only how many cards of each
rank there are. The 13 counts (each 0..4, adding up to n = 5, 6 or 7) are computed for all ranks
at once with the first two steps of a SWAR popcount (each 4-bit group of the mask becomes its
number of set bits). The count vector is then mapped to a dense index by a **combinatorial
ranking of multisets**: its position in lexicographic order among all valid count vectors of the
same total. With `dp[r][k]` = the number of ways to put k cards into r ranks (at most 4 per rank),
the index is

```
index = sum over ranks r of  sum over s < count[r] of  dp[12 - r][remaining_r - s]
```

where `remaining_r` is the number of cards not yet placed in ranks below r: for each smaller
count s at rank r, that many vectors agree on the lower ranks and come first. The inner sums are
precomputed into a 13 × 8 × 5 table (`kIndexOffset`), so the index is 13 table lookups and adds.
It is a minimal perfect hash: every valid count vector gets its own slot, with no gaps and no
collisions, so a plain array indexed by it holds the answer.

**Why 49,205.** That is the number of 7-card rank multisets, i.e. the ways to write 7 as an
ordered sum of 13 counts from 0 to 4. Without the cap of 4, stars and bars gives
C(19, 7) = 50,388. A count of 5 or more is possible on at most one rank; fixing 5 cards on it
leaves 2 cards for 13 ranks, C(14, 2) = 91 ways, so 13 × 91 = 1,183 vectors are excluded:
50,388 − 1,183 = 49,205. The same count gives 6,175 for 5 cards and 18,395 for 6. The number is
checked three times: a `static_assert` on the dp table, a check while the table is built that
enumeration visits exactly that many multisets and fills every slot once, and a unit test that
the index of the k-th vector in lexicographic order is k for every total from 0 to 7.

**Tables.** All tables are built on first use from the reference evaluator (42 ms on an Apple M3,
see below), so the repository holds no binary tables. The flush table takes 16 KiB and the
three non-flush tables 144 KiB (6,175 + 18,395 + 49,205 `uint16` entries). A 7-card lookup
touches at most about 114 KiB: the flush table, the 7-card table (96 KiB) and the 2 KiB offset
table.

**Reference evaluator.** It classifies five cards directly (group the ranks by count, detect
straights including A-2-3-4-5, pack category and tie-break ranks into an integer), and takes the
best of the 6 or 21 five-card subsets for 6 or 7 cards. Sorting the 7,462 distinct scores turns a
score into a `HandRank`.

## Verification

All of these are automated tests (`tests/cpp`, `tests/python`), run in CI.

| check | result |
|---|---|
| all 2,598,960 five-card hands: fast == reference, and `evaluate(mask)` == `evaluate5` | 0 mismatches |
| all 2,598,960 five-card hands: category counts and distinct values | all match (table below), 7,462 values |
| all 133,784,560 seven-card hands (`[exhaustive]`, Release) | all counts match (table below), 4,824 values |
| 1,000,000 random 7-card and 200,000 random 6-card hands (seeded): fast == reference | 0 mismatches |
| rank multiset index for totals 0..7: the k-th vector in lexicographic order gets index k | perfect hash confirmed; 6,175 / 18,395 / 49,205 |
| 100,000 random 5-, 6- and 7-card hands vs phevaluator (PokerHandEvaluator's Python binding) | identical ranks (theirs = 7463 − ours) |
| 20,000 random hands vs treys | identical ranks |
| 100,000 random showdowns vs eval7 | same winner or tie in all; same category for all 200,000 hands |
| exact equity vs a brute force over eval7 (8 spots, 2 to 4 players, flop to river) | equal to 1e-12 |

| category | 5-card hands | 7-card hands |
|---|---:|---:|
| straight flush | 40 (4 royal) | 41,584 (4,324 royal + 37,260 other) |
| four of a kind | 624 | 224,848 |
| full house | 3,744 | 3,473,184 |
| flush | 5,108 | 4,047,644 |
| straight | 10,200 | 6,180,020 |
| three of a kind | 54,912 | 6,461,620 |
| two pair | 123,552 | 31,433,400 |
| one pair | 1,098,240 | 58,627,800 |
| high card | 1,302,540 | 23,294,460 |
| **total** | **2,598,960** | **133,784,560** |
| distinct `HandRank` values | 7,462 | 4,824 |

The exhaustive test prints the 7-card counts as it runs; this is its output on an Apple M3
(Release, 1.75 s):

```
$ ./build/nuts_tests "[exhaustive]"
All 7-card hands: 133784560 hands, 4824 distinct values
  Royal flush                      4324
  Straight flush (other)          37260
  Four of a kind                 224848
  Full house                    3473184
  Flush                         4047644
  Straight                      6180020
  Three of a kind               6461620
  Two pair                     31433400
  One pair                     58627800
  High card                    23294460
...
All tests passed (12 assertions in 1 test case)
```

The unit tests also run under AddressSanitizer + UndefinedBehaviorSanitizer (everything except
the exhaustive test) and ThreadSanitizer (equity, push/fold and Kuhn) in CI. Locally (Apple
clang 21, Debug), ASan + UBSan passed all 67 test cases (2,308,071 assertions) and TSan passed
the 34 equity, push/fold and Kuhn test cases, with no sanitizer reports.

## Equity

[`include/nuts/equity.hpp`](include/nuts/equity.hpp), [`src/equity.cpp`](src/equity.cpp).

**Exact.** `equity_exact(hands, board)` enumerates every completion of the board from the unseen
cards (C(48, 5) = 1,712,304 for two players before the flop), evaluates each player, and counts
wins and k-way ties. A player's equity is the average share of the pot, with a tie split
equally. Everything is kept as integer counts until the end. AhAs vs KdKc preflop:
0.812555 / 0.187445, in 30 ms.

**Monte Carlo.** `equity_monte_carlo(ranges, board, trials, seed, threads)` takes a weighted range
per player. Each trial

1. deals each player a combo from their range with probability proportional to its weight, and
   starts the whole deal over if two players' combos share a card (rejection sampling; re-picking
   only the clashing player would bias the joint distribution). A range that is every combo off
   the board with equal weight (`random`) is instead dealt from the cards left over, which gives
   the same distribution and never fails;
2. deals the rest of the board with a partial Fisher–Yates shuffle, skipping cards already held;
3. evaluates the showdown.

Random numbers come from xoshiro256\*\*, seeded through SplitMix64, with Lemire's unbiased
bounded integers. Thread t uses the generator jumped t times (2^128 steps each) and a fixed
share of the trials, and each thread keeps its counts in a local tally that is merged at the end.
So the result depends only on (inputs, trials, seed, threads), not on scheduling, and is
bit-for-bit reproducible.

**Standard error.** In each trial a player gets a share X of the pot: 1, 1/k for a k-way tie, or
0. The estimate is the mean of X over n trials, and its standard error is
`sqrt(s² / n)` with `s²` the sample variance of X, so it shrinks like 1/√n: 100 times the trials
for one more decimal digit. For AA vs KK (1,000,000 trials, seed 7, 8 threads) the estimate is
0.819604 ± 0.000383; the exact class equity from the committed matrix is 0.819461, 0.37
standard errors away. The tests require Monte Carlo to land within 4 standard errors of exact
enumeration (for hands, three-way pots and weighted ranges with card removal), identical results
for the same seed and thread count, and symmetric results for symmetric matchups.

**Ranges.** `AA`, `AKs`, `AKo`, `AK`, `TT+`, `A2s+`, `22-55`, `K7o-KTo` (either order), specific
combos like `AsKs`, `random`, comma or space separated, each optionally with `:weight`. Tested by
combo counts: `22+` = 78, `AK` = 16, `AKs` = 4, `AKo` = 12, `A2s+` = 48, `random` = 1,326.

## Solvers

### Kuhn poker

[`src/kuhn.cpp`](src/kuhn.cpp). Kuhn's three-card game (J, Q, K; ante 1; one bet of 1) has 12
information sets and a known solution: player 1's value is −1/18 at every Nash equilibrium.
Vanilla CFR walks the full tree for all six deals each iteration, turns cumulative
counterfactual regrets into a strategy by regret matching, and accumulates the reach-weighted
average strategy, which is what converges. Exploitability is computed exactly with best
responses that choose one action per information set (not per opponent card). After 100,000
iterations: game value −0.0555547 (−1/18 = −0.0555556) and exploitability 6.8 × 10⁻⁴; the tests
require both within 10⁻³ and check that player 2's unique equilibrium strategy and player 1's
one-parameter family (bet J with α, bet K with 3α, call with Q after check-bet with α + 1/3) are
found.

### Heads-up push/fold

[`include/nuts/pushfold.hpp`](include/nuts/pushfold.hpp), [`src/pushfold.cpp`](src/pushfold.cpp).
No ante, chip EV. Both players have S big blinds before the blinds (1 ≤ S ≤ 20 in the data file;
the solver accepts up to 100). The SB posts 0.5 and folds (−0.5) or shoves; the BB posts 1 and
folds (SB +1) or calls, after which the SB's result is S · (2e − 1), with e its equity (ties
split).

1. **Equity matrix.** The 1,326 starting hands form 169 classes (13 pairs, 78 suited, 78
   offsuit). For classes i and j the matrix stores `weight(i, j)`, the number of hand pairs with
   no card in common (card removal: AA vs AKs has 6 × 4 = 24 pairs but only 12 are possible), and
   the half pots class i wins over all those pairs and all 1,712,304 boards. Relabeling suits
   does not change a matchup, so the 1,624,350 ordered hand pairs reduce to 47,008 canonical
   matchups (verified independently by `tools/verify/count_matchups.py`), each enumerated
   exactly once: 80.5 billion showdowns. `tools/gen_equity169` does this on all cores: 414 s
   on the M3 below with 8 threads (load average 5.3 at the start and 34 at the end, from
   another job), and the output was byte-for-byte the committed file. The result is committed as
   [`data/equity169.bin`](data/equity169.bin) (228,504 bytes) with its SHA-256. It is exact
   integer data, so the two equities of every pair add up to exactly 1, which the loader checks.
2. **CFR+.** Each player has one decision per class: 169 shove and 169 call probabilities. The
   values of shoving and folding for every class are sums over the opponent's classes weighted by
   the deal probabilities `weight(i, j) / 1,624,350`. Each iteration updates the SB, then the BB
   against the SB's new strategy (alternating updates), with regret matching+ (negative regrets
   reset to 0) and linear averaging (iteration t weighs t). The average strategy is evaluated
   exactly every 50 iterations; the solver stops when its exploitability, the mean gain of the
   two best responses, is at most 10⁻⁶ bb per hand.
3. **Output.** [`data/pushfold.json`](data/pushfold.json) holds 13 × 13 shove and call grids for
   S = 1.0, 1.5, ..., 20.0. Written with 6-decimal probabilities, the largest exploitability is
   1.34 × 10⁻⁶ bb per hand (at 14 bb); each stack took 900 to 2,100 iterations.

| S (bb) | CFR+ iterations | exploitability (bb/hand) | SB EV (bb/hand) | SB shoves | BB calls |
|---:|---:|---:|---:|---:|---:|
| 2 | 1,550 | 6.8e-7 | +0.0095 | 89.6% | 100.0% |
| 5 | 1,350 | 1.1e-6 | +0.0562 | 71.5% | 62.1% |
| 10 | 1,450 | 5.4e-7 | −0.0454 | 58.3% | 37.4% |
| 15 | 1,550 | 6.3e-7 | −0.1272 | 45.7% | 28.6% |
| 20 | 1,850 | 6.8e-7 | −0.1832 | 40.2% | 21.7% |

(Shares are of all 1,326 hands, from `data/pushfold.json`.)

**Comparison with HoldemResources.** [`docs/pushfold_comparison.md`](docs/pushfold_comparison.md)
compares our thresholds, solved on a 0.05 bb grid, with the HoldemResources heads-up no-ante Nash
charts over all 169 hands. All 127 numeric call thresholds and 84 of the 85 numeric shove
thresholds agree within ±0.25 bb, and all 123 "20+" cells agree. The exception, Q4o, comes from a
short second interval above the main one, which matches the chart exactly; the chart says it
leaves out small gaps. Two footnoted hands (63s, 43s) also differ inside their intervals. The
document lists every difference.

Regenerate both data files with

```sh
./build/nuts_gen_equity169 data/equity169.bin
(cd data && shasum -a 256 equity169.bin > equity169.sha256)
./build/nuts_gen_pushfold data/equity169.bin data/pushfold.json
```

The format is documented in [`data/README.md`](data/README.md). The Python and WebAssembly builds
compile the matrix in (CMake turns the file into a C++ array), and a test checks that the
compiled-in copy is byte-for-byte the committed file.

## Benchmarks

`./build/nuts_bench`: each benchmark runs once untimed (warm-up), then 7 timed runs; the table
gives the median. Every run sums its results into a checksum, which must be the same in every
run, so the compiler cannot skip the work. The random hands are generated before timing with a
fixed seed. One thread unless stated.

Apple M3 (4 performance + 4 efficiency cores, 8 hardware threads, 8 GB), macOS, Apple clang
21.0.0 (clang-2100.3.34.2), Release build, `-O3 -DNDEBUG`. Another heavy job was running during
both runs; the load average printed by `nuts_bench` is in the last row.

| benchmark | run 1 | run 2 |
|---|---:|---:|
| table build (first call, once per process) | 42.4 ms | 40.7 ms |
| `evaluate7`, 10,000,000 random hands | 127.5 M evals/s | 123.2 M evals/s |
| `evaluate(mask)`, the same hands as masks | 156.7 M evals/s | 153.4 M evals/s |
| `evaluate7`, all 133,784,560 hands in order | 128.5 M evals/s (1.041 s) | 124.8 M evals/s (1.072 s) |
| `equity_exact`, AhAs vs KdKc preflop (1,712,304 boards) | 30.1 ms | 30.3 ms |
| `equity_monte_carlo`, AA vs random, 10,000,000 trials, 1 thread | 22.3 M trials/s | 26.0 M trials/s |
| the same on 8 threads | 124.0 M trials/s | 118.7 M trials/s |
| load average (1 min) at start | 3.3 | 11.4 |

The only difference between the `evaluate7` and `evaluate(mask)` paths is that `evaluate7` first
builds the mask from seven card ids; both give the same checksum. The 8-thread Monte Carlo
speedup is 4.6× to 5.6×; half of the 8 hardware threads are efficiency cores.

WebAssembly (Emscripten 6.0.10, `-O3`), in Node.js 26.10 on the same machine, single calls timed
by `node wasm/smoke.mjs` (load average about 8): all 133,784,560 seven-card hands in 1.81 s
(74.0 M hands/s), exact AhAs vs KdKc in 55 ms, `pushfold(10)` in 158 ms (first call). Sizes:

| file | bytes | gzip -9 |
|---|---:|---:|
| `nuts.wasm` (228,504 bytes of it are the equity matrix) | 320,068 | 153,284 |
| `nuts.mjs` | 31,968 | 9,393 |

## Layout

```
include/nuts/        public headers
  card.hpp             cards (id = 4 * rank + suit), parsing, 64-bit masks
  evaluator.hpp        HandRank, category(), fast and reference evaluators
  equity.hpp           exact and Monte Carlo equity
  range.hpp            range parser
  rng.hpp              SplitMix64, xoshiro256**
  kuhn.hpp             Kuhn poker CFR
  pushfold.hpp         hand classes, 169 x 169 equity matrix, CFR+ push/fold solver
  embedded_data.hpp    the equity matrix compiled into the binary
src/                 implementations of the above
tools/
  bench.cpp            benchmarks (nuts_bench)
  gen_equity169.cpp    generates data/equity169.bin
  gen_pushfold.cpp     generates data/pushfold.json and the comparison thresholds
  verify/              independent Python cross-checks (RNG constants, 47,008 matchups, eval7)
tests/cpp/           Catch2 tests
tests/python/        pytest tests, including cross-checks against eval7, phevaluator and treys
python/              the Python package (nuts/__init__.py) and its nanobind bindings (src/)
wasm/                embind bindings, Node smoke test, WebAssembly notes
data/                equity169.bin (+ .sha256), pushfold.json, format notes
docs/                pushfold_comparison.md
.github/workflows/   CI: C++ (Linux, macOS), sanitizers, Python 3.10/3.13, WebAssembly
```

## Limitations

- The evaluator covers standard high hands of 5 to 7 cards: no Omaha (2 + 3 selection),
  lowball, hi-lo or short deck.
- Exact equity needs specific hole cards and runs on one thread; ranges go through Monte Carlo.
- Monte Carlo deals overlapping ranges by rejection. That is slow when many wide ranges collide
  and gives up after 1,000,000 failed deals in a row (for example `AA` for three players).
- Push/fold is a model: heads-up only, no ante, chip EV (no ICM), and the SB can only shove or
  fold. Near a threshold many hands are mixed or switch on and off as the stack changes, so a
  threshold read from the strategy depends on the solve precision in its last 0.05 bb.
- The Kuhn solver walks the tree with strings. It is a correctness test for CFR, written for
  clarity, not speed.
- The WebAssembly build is single-threaded: WebAssembly threads need `SharedArrayBuffer`, which a
  page only gets when served with cross-origin isolation headers.
- There is no binary wheel on PyPI; `pip install` compiles the C++ core.
- The benchmark numbers come from one laptop that had another heavy job running at the time.

## References

- Kevin Suffecool, "Cactus Kev's Poker Hand Evaluator" — prime-product hashing of 5-card hands
  and the 7,462 equivalence classes.
- The Two Plus Two forum's 7-card evaluator thread, whose "RayW" state-machine lookup table is
  known as the Two Plus Two evaluator.
- Henry Lee, PokerHandEvaluator, <https://github.com/HenryRLee/PokerHandEvaluator> — the flush
  table + rank-multiset perfect hash scheme used here; its Python binding, phevaluator, is one of
  the test oracles.
- M. Zinkevich, M. Johanson, M. Bowling, C. Piccione, "Regret Minimization in Games with
  Incomplete Information", NIPS 2007 (CFR).
- O. Tammelin, "Solving Large Imperfect Information Games Using CFR+", arXiv:1407.5042, 2014.
- H. W. Kuhn, "A Simplified Two-Person Poker", Contributions to the Theory of Games I, 1950.
- D. Blackman, S. Vigna, "Scrambled Linear Pseudorandom Number Generators", 2018
  (xoshiro256\*\*), <https://prng.di.unimi.it/>.
- D. Lemire, "Fast Random Integer Generation in an Interval", ACM TOMACS, 2019.
- HoldemResources.net, "HeadsUp Push/Fold Nash Equilibrium", <https://www.holdemresources.net/hune>.
- Test oracles: eval7 (<https://github.com/julianandrews/pyeval7>) and treys.

## License

MIT, see [LICENSE](LICENSE).
