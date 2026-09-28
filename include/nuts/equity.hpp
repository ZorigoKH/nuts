// Equity: each player's expected share of the pot when all the cards are dealt and nobody folds.
//
// At showdown the best hand wins the pot; if k players share the best hand, each gets 1/k of it.
// A player's equity is the average share over all the ways the unknown cards can come.
//
// Two ways to compute it:
//   * equity_exact(): every player's hole cards are known. Goes through every possible completion
//     of the board (C(48, 5) = 1,712,304 of them for two players before the flop) and counts.
//   * equity_monte_carlo(): every player has a range. Deals random hole cards from the ranges and
//     a random rest of the board many times and averages; the result has a random error, which
//     is estimated and reported as a standard error.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/range.hpp"

namespace nuts {

// Every player's two hole cards plus a five-card board must fit in the deck: 2 * 23 + 5 <= 52.
inline constexpr int kMaxPlayers = 23;

// The most threads equity_monte_carlo() and compute_equity169() (pushfold.hpp) will use. Far
// more than a CPU has cores, and far below the number of threads an operating system lets one
// program start (a few thousand), so asking for too many is an error rather than a crash.
inline constexpr int kMaxThreads = 256;

struct PlayerEquity {
  double win = 0;        // probability of winning the whole pot
  double tie = 0;        // probability of sharing the pot with one or more other players
  double equity = 0;     // expected share of the pot: win + the average share in the ties
  double std_error = 0;  // standard error of `equity` for Monte Carlo; 0 for exact results
};

struct EquityResult {
  std::vector<PlayerEquity> players;  // in the order the hands or ranges were given
  std::uint64_t deals = 0;            // boards enumerated (exact) or trials simulated (Monte Carlo)
  int threads = 1;                    // threads used
  bool exact = false;
};

// Exact equity for 2..23 players with known hole cards. `board` holds the 0..5 known board cards;
// every way to deal the rest of the board from the unseen cards is counted once.
//
// Returns std::nullopt, and stores a message in *error if error is not null, when the input is
// invalid: fewer than 2 or more than 23 players, a card id >= 52, a card used twice, or more than
// 5 board cards.
std::optional<EquityResult> equity_exact(std::span<const HoleCards> hands,
                                         std::span<const Card> board, std::string* error = nullptr);

// Monte Carlo equity for 2..23 players given as ranges (see range.hpp). Each trial deals every
// player a combo from their range, with probability proportional to its weight, such that no
// card is used twice and none is on the board; then deals the rest of the board at random.
//
//   trials   number of trials (at least 1). The standard error shrinks like 1/sqrt(trials).
//   seed     seeds the random numbers.
//   threads  number of threads, at most kMaxThreads (256); 0 (or less) means one per hardware
//            thread (at most kMaxThreads). It is capped at `trials`. The result depends only on
//            (inputs, trials, seed, threads used): the same values give bit-for-bit the same
//            result. EquityResult::threads reports the count used.
//
// How the hole cards are dealt: a player whose range is every combo that avoids the board, all
// with the same weight (such as "random"), gets two cards from the deck left over after the
// other players. Those other players are dealt from their ranges by trial and error: if two of
// them get a common card, the deal starts again. That gets slow when many wide ranges overlap,
// and gives up after 1,000,000 failed deals in a row (for example "AA" for three players).
//
// Returns std::nullopt, and stores a message in *error if error is not null, when the input is
// invalid: fewer than 2 or more than 23 players, trials == 0, more than kMaxThreads threads, bad
// board cards, a combo with a bad or repeated card or a weight that is negative or not finite, a
// range with no combo left once the combos that use board cards are removed, or ranges that
// could not be dealt together as just described. Also if the operating system refuses to start
// the threads.
std::optional<EquityResult> equity_monte_carlo(std::span<const Range> ranges,
                                               std::span<const Card> board, std::uint64_t trials,
                                               std::uint64_t seed, int threads,
                                               std::string* error = nullptr);

}  // namespace nuts
