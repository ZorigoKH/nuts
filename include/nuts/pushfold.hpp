// Heads-up push/fold poker: the 169 starting-hand classes, their preflop equity matrix, and a
// CFR+ solver for the Nash equilibrium.
//
// The game (no ante, chip EV, everything in big blinds):
//
//   * Both players start with S big blinds (the effective stack, counted before the blinds are
//     posted). The small blind (SB) posts 0.5, the big blind (BB) posts 1.
//   * The SB either folds (and loses the 0.5 it posted) or goes all-in ("shoves").
//   * Facing a shove, the BB either folds (and the SB wins the BB's 1) or calls. After a call the
//     five board cards are dealt and the better hand wins both stacks: the SB's result is
//     S * (2e - 1), where e is the SB's equity (its share of the pot; ties split it).
//
// Each player's only information is its own two cards, and for this game only the "hand class"
// matters (see below), so a strategy is one probability per hand class: how often the SB shoves
// and how often the BB calls.
//
// Hand classes. Before the flop the suits themselves do not matter, only whether the two cards
// share a suit. So the 1,326 two-card hands fall into 169 classes: 13 pairs ("AA", 6 hands
// each), 78 suited hands ("AKs", 4 each) and 78 offsuit hands ("AKo", 12 each). They are
// numbered like the cells of the usual 13 x 13 chart, rows and columns in the order A, K, ..., 2:
//
//     index = 13 * row + column
//     row == column    a pair           ("AA" = 0, "KK" = 14, ..., "22" = 168)
//     row <  column    a suited hand    (row = higher rank: "AKs" = 1, "A2s" = 12)
//     row >  column    an offsuit hand  (column = higher rank: "AKo" = 13, "32o" = 167)
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/range.hpp"

namespace nuts {

// =============================================================================================
// Hand classes.
// =============================================================================================

inline constexpr int kNumHandClasses = 169;
inline constexpr int kGridSize = 13;  // the chart is kGridSize x kGridSize

// The class of two hole cards (valid, different cards).
int hand_class_of(const HoleCards& cards) noexcept;

// "AA", "AKs", "AKo", ... for an index in 0..168.
std::string hand_class_name(int hand_class);

// The index of a class name such as "AA", "AKs" or "72o" (either case, and the ranks in either
// order: "KAs" = "AKs"); std::nullopt if it is not one of the 169 classes. A non-pair needs its
// s or o suffix.
std::optional<int> parse_hand_class(std::string_view name);

// All two-card hands in the class: 6 for a pair, 4 suited, 12 offsuit.
std::vector<HoleCards> hand_class_combos(int hand_class);

// 6, 4 or 12.
int hand_class_num_combos(int hand_class) noexcept;

// =============================================================================================
// The 169 x 169 preflop equity matrix.
// =============================================================================================

// Number of boards for one heads-up matchup before the flop: C(48, 5).
inline constexpr std::uint32_t kPreflopBoards = 1'712'304;

// Number of ordered pairs of two-card hands that share no card: 1,326 * C(50, 2).
inline constexpr std::uint32_t kPreflopMatchups = 1'326 * 1'225;

// The equity of every hand class against every other, with card removal.
//
// For classes i and j, look at every pair (hand a in class i, hand b in class j) whose four
// cards are all different, and at every one of the C(48, 5) boards for that pair. Two integers
// per (i, j) describe the result exactly:
//
//   weight(i, j)     the number of such hand pairs. It is smaller than
//                    num_combos(i) * num_combos(j) when the classes share a rank: AA vs AKs
//                    has 6 * 4 = 24 pairs of hands, but only 12 of them have no card in common
//                    (each AA leaves 2 aces, so 2 of the 4 AKs hands).
//                    This is the "card removal" effect: holding an ace makes it less likely
//                    that the opponent holds one.
//   half_pots(i, j)  the number of half pots won by the class-i hand, summed over all those hand
//                    pairs and all boards: 2 for each board it wins, 1 for each tie.
//
// equity(i, j) = half_pots / (2 * kPreflopBoards * weight) is the class-i hand's average share
// of the pot. Every hand pair shows up once from each side, so weight(i, j) == weight(j, i) and
// half_pots(i, j) + half_pots(j, i) == 2 * kPreflopBoards * weight(i, j), i.e. the equities of
// i against j and of j against i add up to exactly 1.
//
// The matrix is computed once by tools/gen_equity169 (compute_equity169() below) and stored in
// data/equity169.bin (format: see data/README.md).
struct Equity169 {
  std::vector<std::uint32_t> weights;    // 169 * 169, row i = class i, column j = class j
  std::vector<std::uint32_t> half_pots;  // 169 * 169, same layout

  std::uint32_t weight(int i, int j) const { return weights[index(i, j)]; }
  std::uint32_t half_pots_won(int i, int j) const { return half_pots[index(i, j)]; }
  // The equity of class i against class j (0 if they cannot both be dealt, which never happens:
  // even AA vs AA has 6 hand pairs, each AA against the one made of the other two aces).
  double equity(int i, int j) const;

  static std::size_t index(int i, int j) {
    return static_cast<std::size_t>(i) * kNumHandClasses + static_cast<std::size_t>(j);
  }
};

// Called while compute_equity169() runs, after each matchup is done, with the number done so far
// and the total. Calls never overlap (they are made one at a time, under a lock), but they may
// come from different threads.
using ProgressCallback = std::function<void(std::size_t done, std::size_t total)>;

// Computes the matrix exactly. Uses suit symmetry so that each distinct matchup is enumerated
// once (47,008 of them, see detail::canonical_matchup()), each over all C(48, 5) boards, spread
// over `threads` threads (0 or less = one per hardware thread; at most kMaxThreads = 256, from
// equity.hpp; fewer if the operating system will not start that many). The result does not
// depend on the number of threads. Takes minutes; see tools/gen_equity169.cpp.
Equity169 compute_equity169(int threads, const ProgressCallback& progress = {});

// The file format (see data/README.md): a 16-byte header, then the weights and the half-pot
// counts as little-endian 32-bit integers.
std::vector<std::uint8_t> equity169_to_bytes(const Equity169& equity);

// Reads the file format back. Returns std::nullopt, with a message in *error if error is not
// null, if the bytes are not a valid file: wrong header or size, or counts that are inconsistent
// (see the checks listed in the .cpp file).
std::optional<Equity169> equity169_from_bytes(std::span<const std::uint8_t> bytes,
                                              std::string* error = nullptr);

// Reads or writes a file (e.g. "data/equity169.bin"). Same error convention as above.
std::optional<Equity169> load_equity169(const std::string& path, std::string* error = nullptr);
bool save_equity169(const Equity169& equity, const std::string& path, std::string* error = nullptr);

// =============================================================================================
// The push/fold game and its solver.
// =============================================================================================

// Probabilities for each of the 169 hand classes (index as above).
struct PushFoldStrategy {
  std::vector<double> shove = std::vector<double>(kNumHandClasses, 0.0);  // SB goes all-in
  std::vector<double> call = std::vector<double>(kNumHandClasses, 0.0);   // BB calls the shove
};

// How good a pair of strategies is. All values are in big blinds per hand, averaged over all
// deals (the SB's hand, the BB's hand and the board).
//
// A best response is the most a player can win by changing only its own strategy, knowing the
// other's. At a Nash equilibrium neither player can gain by that, so the exploitability,
//
//     ((sb_best_response - sb_value) + (bb_best_response - bb_value)) / 2,
//
// the average of what the two best responses gain (with bb_value = -sb_value, since what one
// player wins the other loses), is 0 exactly at an equilibrium and small near one.
struct PushFoldEvaluation {
  double sb_value = 0;          // the SB's expected result when both play the given strategies
  double sb_best_response = 0;  // the SB's expected result with a best response to the BB
  double bb_best_response = 0;  // the BB's expected result with a best response to the SB
  double exploitability = 0;
};

// Evaluates `strategy` at effective stack `stack_bb` (at least 1). The probabilities must be
// in [0, 1] (they are not checked).
PushFoldEvaluation evaluate_pushfold(const Equity169& equity, double stack_bb,
                                     const PushFoldStrategy& strategy);

// The largest effective stack solve_pushfold() accepts, in big blinds. Push/fold is only played
// up to about 20 bb; the cap keeps a solve short, since CFR+ needs more iterations for deeper
// stacks (1,850 at 20 bb and 8,150 at 100 bb: 0.05 s and 0.2 s on an Apple M3, Release).
inline constexpr double kMaxStackBb = 100;

struct PushFoldOptions {
  // CFR+ stops as soon as the exploitability of its average strategy is at most this (checked
  // every `check_every` iterations), or after max_iterations.
  double target_exploitability = 1e-6;
  int max_iterations = 1'000'000;
  int check_every = 50;
};

struct PushFoldSolution {
  double stack_bb = 0;
  PushFoldStrategy strategy;      // the average strategy of CFR+
  PushFoldEvaluation evaluation;  // of `strategy`
  int iterations = 0;             // CFR+ iterations run
  // True if the exploitability reached options.target_exploitability; false if the solver
  // stopped at options.max_iterations first.
  bool converged = false;
};

// Solves the game at effective stack `stack_bb` with CFR+ (see src/pushfold.cpp). Returns
// std::nullopt, with a message in *error if error is not null, if stack_bb is not a number from 1
// to kMaxStackBb or the options are not positive. Check `converged` in the result: with the
// default options every stack from 1 to kMaxStackBb converges.
std::optional<PushFoldSolution> solve_pushfold(const Equity169& equity, double stack_bb,
                                               const PushFoldOptions& options = {},
                                               std::string* error = nullptr);

// =============================================================================================
// Internals, exposed for the tests.
// =============================================================================================
namespace detail {

// A heads-up matchup up to suit symmetry. Relabeling the suits (e.g. swapping hearts and spades
// everywhere) does not change who wins on any board, so it does not change equity. `key` is the
// same for two matchups exactly when one becomes the other by relabeling suits, possibly with
// the two players swapped; `swapped` says whether the player order was swapped to get the
// canonical form.
struct CanonicalMatchup {
  std::uint32_t key = 0;
  bool swapped = false;
};

// Precondition: a and b are valid hole cards with no card in common.
CanonicalMatchup canonical_matchup(const HoleCards& a, const HoleCards& b) noexcept;

// The two hands of a canonical matchup (a, b) from its key: canonical_matchup(a, b) has the same
// key and swapped == false.
std::array<HoleCards, 2> decode_matchup(std::uint32_t key) noexcept;

}  // namespace detail

}  // namespace nuts
