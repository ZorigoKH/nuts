// Push/fold: hand classes, the 169 x 169 equity matrix and the CFR+ solver. See
// include/nuts/pushfold.hpp for the game and the definitions. The file has four parts:
//
//   1. Hand classes.
//   2. Computing the equity matrix (with suit symmetry).
//   3. Reading and writing the matrix file.
//   4. Evaluating strategies (exploitability) and solving the game with CFR+.
#include "nuts/pushfold.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <thread>

#include "nuts/equity.hpp"

namespace nuts {
namespace {

// Stores the message (if error is not null); returns std::nullopt so callers can write
// `return fail(error, "...");`.
std::nullopt_t fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return std::nullopt;
}

// =============================================================================================
// Part 1: hand classes.
// =============================================================================================

// Row (or column) r of the chart holds rank kAce - r: row 0 is the ace, row 12 the deuce.
constexpr int rank_of_row(int row) { return kAce - row; }
constexpr int row_of_rank(int rank) { return kAce - rank; }

// Number of two-card hands with no card in common with a given hand: C(50, 2).
constexpr std::uint32_t kOpponentHands = 1'225;

// Calls visit(a, b) for every ordered pair of two-card hands with no card in common
// (kPreflopMatchups = 1,624,350 pairs).
template <typename Visit>
void for_each_matchup(Visit&& visit) {
  std::vector<HoleCards> hands;
  for (int high = kNumCards - 1; high >= 0; --high) {
    for (int low = high - 1; low >= 0; --low) {
      hands.push_back(make_hole_cards(static_cast<Card>(high), static_cast<Card>(low)));
    }
  }
  for (const HoleCards& a : hands) {
    const std::uint64_t mask_a = card_bit(a[0]) | card_bit(a[1]);
    for (const HoleCards& b : hands) {
      if ((card_bit(b[0]) | card_bit(b[1])) & mask_a) continue;  // a card in common
      visit(a, b);
    }
  }
}

// =============================================================================================
// Part 2: computing the equity matrix.
//
// Doing it directly would mean enumerating C(48, 5) boards for each of the 812,175 unordered
// pairs of hands: hours. But most pairs are the same matchup with the suits renamed. AhKh vs
// QsQd and AsKs vs QhQd are the same situation (rename hearts <-> spades), so they have the same
// equity. We therefore
//
//   1. give every ordered pair of hands a canonical form (detail::canonical_matchup): of all the
//      ways to rename the 4 suits (24 permutations) and to list the two players (2 orders), the
//      one with the smallest code. Pairs with the same canonical form have the same equity (seen
//      from the right side). There are 47,008 different canonical forms.
//   2. enumerate the boards once per canonical form (equity_exact), in parallel threads;
//   3. go over all ordered pairs of hands again and add each pair's result, looked up by its
//      canonical form, to its (class, class) cell of the matrix.
// =============================================================================================

// The 24 ways to rename the suits: kSuitPermutations[p][s] is the new name of suit s.
constexpr std::array<std::array<int, kNumSuits>, 24> kSuitPermutations = [] {
  std::array<std::array<int, kNumSuits>, 24> permutations{};
  std::array<int, kNumSuits> permutation = {0, 1, 2, 3};
  for (auto& out : permutations) {
    out = permutation;
    std::next_permutation(permutation.begin(), permutation.end());
  }
  return permutations;
}();

HoleCards rename_suits(const HoleCards& hand, const std::array<int, kNumSuits>& permutation) {
  const auto rename = [&](Card card) {
    return make_card(rank_of(card), permutation[static_cast<std::size_t>(suit_of(card))]);
  };
  return make_hole_cards(rename(hand[0]), rename(hand[1]));
}

// A number for an ordered pair of hands: the four card ids as the digits of a base-52 number
// (below 52^4 = 7,311,616).
std::uint32_t encode_matchup(const HoleCards& a, const HoleCards& b) {
  return ((std::uint32_t{a[0]} * kNumCards + a[1]) * kNumCards + b[0]) * kNumCards + b[1];
}

// The integer results of one matchup: out of kPreflopBoards boards, how many the first hand wins
// and on how many the two hands tie. (The second hand wins the rest.)
struct MatchupCounts {
  std::uint32_t wins = 0;
  std::uint32_t ties = 0;
};

MatchupCounts count_matchup(const std::array<HoleCards, 2>& hands) {
  const std::optional<EquityResult> result = equity_exact(hands, {});
  // equity_exact() only fails for invalid input, and a decoded matchup is always valid.
  if (!result || result->deals != kPreflopBoards) return {};
  // equity_exact() reports probabilities, count / deals. Multiplying back and rounding recovers
  // the integer count exactly: the rounding errors of the division and the multiplication are
  // far below 0.5 for counts this small (below 2^21).
  const auto deals = static_cast<double>(result->deals);
  return {static_cast<std::uint32_t>(std::llround(result->players[0].win * deals)),
          static_cast<std::uint32_t>(std::llround(result->players[0].tie * deals))};
}

// =============================================================================================
// Part 3: the file format (also described in data/README.md).
//
//   offset  size             contents
//   0       8                the characters "NUTSE169"
//   8       4                number of hand classes (169)
//   12      4                boards per matchup (1,712,304)
//   16      169 * 169 * 4    weights, row by row (weight(0, 0), weight(0, 1), ...)
//   114260  169 * 169 * 4    half-pot counts, same order
//   228504                   end of file
//
// All integers are unsigned 32-bit little-endian, written byte by byte so the file is the same
// on every machine.
// =============================================================================================

constexpr std::string_view kMagic = "NUTSE169";
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kMatrixCells = std::size_t{kNumHandClasses} * kNumHandClasses;
constexpr std::size_t kFileSize = kHeaderSize + 2 * kMatrixCells * 4;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) {
    out.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
  }
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int byte = 0; byte < 4; ++byte) {
    value |= std::uint32_t{bytes[offset + static_cast<std::size_t>(byte)]} << (8 * byte);
  }
  return value;
}

// =============================================================================================
// Part 4: the game and CFR+.
// =============================================================================================

// The push/fold game at one stack size, with the matrix turned into the two numbers per
// (SB class i, BB class j) that the payoffs need:
//
//   deal_[i][j]    = weight(i, j) / kPreflopMatchups: the probability that the SB is dealt a
//                    class-i hand and the BB a class-j hand;
//   called_[i][j]  = deal_[i][j] * S * (2 * equity(i, j) - 1): the same, times the SB's average
//                    result when the BB calls.
//
// The value functions below return, for each hand class, the expected result of each action
// summed over the opponent's classes and weighted by these deal probabilities. That is exactly
// the "counterfactual value" CFR works with: a player's decision with class i only matters in
// the deals where it holds class i, weighted by how likely each opponent class is then.
class PushFoldGame {
 public:
  PushFoldGame(const Equity169& equity, double stack_bb)
      : deal_(kMatrixCells), called_(kMatrixCells) {
    for (int i = 0; i < kNumHandClasses; ++i) {
      for (int j = 0; j < kNumHandClasses; ++j) {
        const std::size_t ij = Equity169::index(i, j);
        deal_[ij] = static_cast<double>(equity.weight(i, j)) / kPreflopMatchups;
        called_[ij] = deal_[ij] * stack_bb * (2 * equity.equity(i, j) - 1);
      }
    }
  }

  // The SB's values of shoving and of folding with each class, against the BB's call
  // probabilities `call`:
  //   shove: BB folds (prob 1 - call[j]) -> SB wins 1;  BB calls (prob call[j]) -> S(2e - 1)
  //   fold:  SB loses its 0.5
  void sb_values(std::span<const double> call, std::vector<double>& shove_value,
                 std::vector<double>& fold_value) const {
    for (int i = 0; i < kNumHandClasses; ++i) {
      double shove = 0;
      double deal_total = 0;
      for (int j = 0; j < kNumHandClasses; ++j) {
        const std::size_t ij = Equity169::index(i, j);
        const double c = call[static_cast<std::size_t>(j)];
        shove += deal_[ij] * (1 - c) + called_[ij] * c;
        deal_total += deal_[ij];
      }
      shove_value[static_cast<std::size_t>(i)] = shove;
      fold_value[static_cast<std::size_t>(i)] = -0.5 * deal_total;
    }
  }

  // The BB's values of calling and of folding with each class, facing a shove, against the
  // SB's shove probabilities `shove` (the BB only decides when the SB has shoved):
  //   call: BB's result is minus the SB's, -S(2e - 1)
  //   fold: BB loses its 1
  void bb_values(std::span<const double> shove, std::vector<double>& call_value,
                 std::vector<double>& fold_value) const {
    std::fill(call_value.begin(), call_value.end(), 0.0);
    std::fill(fold_value.begin(), fold_value.end(), 0.0);
    for (int i = 0; i < kNumHandClasses; ++i) {
      const double s = shove[static_cast<std::size_t>(i)];
      for (int j = 0; j < kNumHandClasses; ++j) {
        const std::size_t ij = Equity169::index(i, j);
        call_value[static_cast<std::size_t>(j)] -= s * called_[ij];
        fold_value[static_cast<std::size_t>(j)] -= s * deal_[ij];
      }
    }
  }

  // The BB's result from the deals where the SB folds: it wins the SB's 0.5.
  double bb_value_when_sb_folds(std::span<const double> shove) const {
    double value = 0;
    for (int i = 0; i < kNumHandClasses; ++i) {
      double deal_total = 0;
      for (int j = 0; j < kNumHandClasses; ++j) deal_total += deal_[Equity169::index(i, j)];
      value += (1 - shove[static_cast<std::size_t>(i)]) * 0.5 * deal_total;
    }
    return value;
  }

  PushFoldEvaluation evaluate(const PushFoldStrategy& strategy) const {
    std::vector<double> value_a(kNumHandClasses);
    std::vector<double> value_b(kNumHandClasses);
    PushFoldEvaluation result;

    // The SB's value, and its best response: with each class, the better of its two actions.
    sb_values(strategy.call, value_a, value_b);  // a = shove, b = fold
    for (std::size_t i = 0; i < kNumHandClasses; ++i) {
      const double s = strategy.shove[i];
      result.sb_value += s * value_a[i] + (1 - s) * value_b[i];
      result.sb_best_response += std::max(value_a[i], value_b[i]);
    }

    // The BB's best response: the better action with each class when facing a shove, plus what
    // it gets when the SB folds (which the BB cannot change).
    bb_values(strategy.shove, value_a, value_b);  // a = call, b = fold
    result.bb_best_response = bb_value_when_sb_folds(strategy.shove);
    for (std::size_t j = 0; j < kNumHandClasses; ++j) {
      result.bb_best_response += std::max(value_a[j], value_b[j]);
    }

    const double bb_value = -result.sb_value;
    result.exploitability =
        ((result.sb_best_response - result.sb_value) + (result.bb_best_response - bb_value)) / 2;
    return result;
  }

 private:
  std::vector<double> deal_;
  std::vector<double> called_;
};

// Regret matching for a decision with two actions: play each action in proportion to its
// positive cumulative regret (in CFR+ the regrets are never negative); if both are 0, play both
// equally often. Returns the probability of the first action.
double regret_matching(double regret_first, double regret_second) {
  const double total = regret_first + regret_second;
  return total > 0 ? regret_first / total : 0.5;
}

// One CFR+ regret update for one player, for all 169 classes. `probability[i]` is how often the
// player took the first action with class i this iteration; value_first/second are the values of
// the two actions. Each action's regret grows by how much better it was than the strategy played,
// and CFR+ then resets negative regrets to 0 (the "+" in CFR+): an action that did badly in the
// past can come back as soon as it starts doing well, instead of first paying off its debt.
void update_regrets(std::span<const double> probability, std::span<const double> value_first,
                    std::span<const double> value_second, std::vector<double>& regret_first,
                    std::vector<double>& regret_second) {
  for (std::size_t i = 0; i < kNumHandClasses; ++i) {
    const double played = probability[i] * value_first[i] + (1 - probability[i]) * value_second[i];
    regret_first[i] = std::max(regret_first[i] + value_first[i] - played, 0.0);
    regret_second[i] = std::max(regret_second[i] + value_second[i] - played, 0.0);
  }
}

}  // namespace

// ---- Part 1: hand classes -------------------------------------------------------------------

int hand_class_of(const HoleCards& cards) noexcept {
  const int row_a = row_of_rank(rank_of(cards[0]));
  const int row_b = row_of_rank(rank_of(cards[1]));
  const int high_row = std::min(row_a, row_b);  // the higher rank has the smaller row
  const int low_row = std::max(row_a, row_b);
  if (suit_of(cards[0]) == suit_of(cards[1])) {
    return kGridSize * high_row + low_row;  // suited: above the diagonal
  }
  return kGridSize * low_row + high_row;  // offsuit (or a pair, where the two rows are equal)
}

std::string hand_class_name(int hand_class) {
  const int row = hand_class / kGridSize;
  const int column = hand_class % kGridSize;
  const auto rank_char = [](int r) { return kRankChars[static_cast<std::size_t>(rank_of_row(r))]; };
  if (row == column) return {rank_char(row), rank_char(row)};
  if (row < column) return {rank_char(row), rank_char(column), 's'};
  return {rank_char(column), rank_char(row), 'o'};
}

std::optional<int> parse_hand_class(std::string_view name) {
  if (name.size() != 2 && name.size() != 3) return std::nullopt;
  const std::optional<int> first = parse_rank(name[0]);
  const std::optional<int> second = parse_rank(name[1]);
  if (!first || !second) return std::nullopt;
  const int high_row = row_of_rank(std::max(*first, *second));
  const int low_row = row_of_rank(std::min(*first, *second));
  if (name.size() == 2) {
    if (high_row != low_row) return std::nullopt;  // "AK" is two classes, AKs and AKo
    return kGridSize * high_row + high_row;
  }
  if (high_row == low_row) return std::nullopt;  // "AAs" is not a hand
  const char suffix = name[2];
  if (suffix == 's' || suffix == 'S') return kGridSize * high_row + low_row;
  if (suffix == 'o' || suffix == 'O') return kGridSize * low_row + high_row;
  return std::nullopt;
}

std::vector<HoleCards> hand_class_combos(int hand_class) {
  const int row = hand_class / kGridSize;
  const int column = hand_class % kGridSize;
  const int rank_a = rank_of_row(row);
  const int rank_b = rank_of_row(column);
  std::vector<HoleCards> combos;
  for (int suit_a = 0; suit_a < kNumSuits; ++suit_a) {
    for (int suit_b = 0; suit_b < kNumSuits; ++suit_b) {
      const bool keep = row == column  ? suit_a < suit_b    // pair: each pair of suits once
                        : row < column ? suit_a == suit_b   // suited
                                       : suit_a != suit_b;  // offsuit
      if (keep) {
        combos.push_back(make_hole_cards(make_card(rank_a, suit_a), make_card(rank_b, suit_b)));
      }
    }
  }
  return combos;
}

int hand_class_num_combos(int hand_class) noexcept {
  const int row = hand_class / kGridSize;
  const int column = hand_class % kGridSize;
  return row == column ? 6 : row < column ? 4 : 12;
}

// ---- Part 2: the equity matrix --------------------------------------------------------------

double Equity169::equity(int i, int j) const {
  const std::uint32_t w = weight(i, j);
  if (w == 0) return 0;
  return static_cast<double>(half_pots_won(i, j)) / (2.0 * kPreflopBoards * w);
}

namespace detail {

CanonicalMatchup canonical_matchup(const HoleCards& a, const HoleCards& b) noexcept {
  CanonicalMatchup best{UINT32_MAX, false};
  for (const auto& permutation : kSuitPermutations) {
    const HoleCards renamed_a = rename_suits(a, permutation);
    const HoleCards renamed_b = rename_suits(b, permutation);
    const std::uint32_t forward = encode_matchup(renamed_a, renamed_b);
    const std::uint32_t backward = encode_matchup(renamed_b, renamed_a);
    if (forward < best.key) best = {forward, false};
    if (backward < best.key) best = {backward, true};
  }
  return best;
}

std::array<HoleCards, 2> decode_matchup(std::uint32_t key) noexcept {
  std::array<Card, 4> cards{};
  for (int k = 3; k >= 0; --k) {
    cards[static_cast<std::size_t>(k)] = static_cast<Card>(key % kNumCards);
    key /= kNumCards;
  }
  return {HoleCards{cards[0], cards[1]}, HoleCards{cards[2], cards[3]}};
}

}  // namespace detail

Equity169 compute_equity169(int threads, const ProgressCallback& progress) {
  // Step 1: the canonical form of every ordered pair of hands; keep each form once, sorted.
  std::vector<std::uint32_t> keys;
  keys.reserve(kPreflopMatchups);
  for_each_matchup([&](const HoleCards& a, const HoleCards& b) {
    keys.push_back(detail::canonical_matchup(a, b).key);
  });
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

  // Step 2: enumerate the boards of each canonical matchup. The threads take matchups one at a
  // time from a shared counter (so a thread that got fast matchups simply takes more), and each
  // writes only to its own matchups' slots in `counts`.
  std::vector<MatchupCounts> counts(keys.size());
  std::atomic<std::size_t> next{0};
  std::mutex progress_mutex;
  std::size_t done = 0;  // guarded by progress_mutex
  auto work = [&] {
    for (std::size_t k = next++; k < keys.size(); k = next++) {
      counts[k] = count_matchup(detail::decode_matchup(keys[k]));
      const std::lock_guard<std::mutex> lock(progress_mutex);
      ++done;
      if (progress) progress(done, keys.size());
    }
  };
  // This thread works too, so num_threads - 1 more are started. If the operating system refuses
  // to start one (std::system_error), carry on with those already running: they all take
  // matchups from the shared counter until none are left, so the result is the same.
  const int hardware_threads = static_cast<int>(std::thread::hardware_concurrency());
  const int num_threads = std::clamp(threads > 0 ? threads : hardware_threads, 1, kMaxThreads);
  std::vector<std::thread> workers;
  for (int t = 1; t < num_threads; ++t) {
    try {
      workers.emplace_back(work);
    } catch (const std::system_error&) {
      break;
    }
  }
  work();
  for (std::thread& worker : workers) worker.join();

  // Step 3: add every ordered pair of hands to its cell. A pair whose canonical form lists the
  // players the other way round gets the second hand's counts.
  Equity169 equity;
  equity.weights.assign(kMatrixCells, 0);
  equity.half_pots.assign(kMatrixCells, 0);
  for_each_matchup([&](const HoleCards& a, const HoleCards& b) {
    const detail::CanonicalMatchup canonical = detail::canonical_matchup(a, b);
    const auto k = static_cast<std::size_t>(
        std::lower_bound(keys.begin(), keys.end(), canonical.key) - keys.begin());
    const MatchupCounts& c = counts[k];
    const std::uint32_t losses = kPreflopBoards - c.wins - c.ties;
    const std::uint32_t wins_of_a = canonical.swapped ? losses : c.wins;
    const std::size_t cell = Equity169::index(hand_class_of(a), hand_class_of(b));
    equity.weights[cell] += 1;
    equity.half_pots[cell] += 2 * wins_of_a + c.ties;
  });
  return equity;
}

// ---- Part 3: the file format ----------------------------------------------------------------

std::vector<std::uint8_t> equity169_to_bytes(const Equity169& equity) {
  std::vector<std::uint8_t> bytes(kMagic.begin(), kMagic.end());
  bytes.reserve(kFileSize);
  append_u32(bytes, kNumHandClasses);
  append_u32(bytes, kPreflopBoards);
  for (const std::uint32_t w : equity.weights) append_u32(bytes, w);
  for (const std::uint32_t h : equity.half_pots) append_u32(bytes, h);
  return bytes;
}

std::optional<Equity169> equity169_from_bytes(std::span<const std::uint8_t> bytes,
                                              std::string* error) {
  if (bytes.size() != kFileSize) {
    return fail(error, "equity matrix: expected " + std::to_string(kFileSize) + " bytes, got " +
                           std::to_string(bytes.size()));
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return fail(error, "equity matrix: the file does not start with \"NUTSE169\"");
  }
  if (read_u32(bytes, 8) != kNumHandClasses || read_u32(bytes, 12) != kPreflopBoards) {
    return fail(error, "equity matrix: unexpected class or board count in the header");
  }
  Equity169 equity;
  equity.weights.resize(kMatrixCells);
  equity.half_pots.resize(kMatrixCells);
  for (std::size_t cell = 0; cell < kMatrixCells; ++cell) {
    equity.weights[cell] = read_u32(bytes, kHeaderSize + 4 * cell);
    equity.half_pots[cell] = read_u32(bytes, kHeaderSize + 4 * (kMatrixCells + cell));
  }

  // Consistency checks, so a damaged or wrong file is caught here rather than giving wrong
  // answers later:
  //   * each hand of class i meets each of the C(50, 2) = 1,225 hands that share no card with
  //     it exactly once, so row i of the weights adds up to num_combos(i) * 1,225;
  //   * weights are symmetric, and the half pots of (i, j) and (j, i) add up to 2 per board.
  for (int i = 0; i < kNumHandClasses; ++i) {
    std::uint64_t row_total = 0;
    for (int j = 0; j < kNumHandClasses; ++j) {
      const std::uint64_t w = equity.weight(i, j);
      row_total += w;
      if (w != equity.weight(j, i)) {
        return fail(error, "equity matrix: the weights are not symmetric at " + hand_class_name(i) +
                               " vs " + hand_class_name(j));
      }
      const std::uint64_t half_pots =
          std::uint64_t{equity.half_pots_won(i, j)} + equity.half_pots_won(j, i);
      if (half_pots != 2 * std::uint64_t{kPreflopBoards} * w) {
        return fail(error, "equity matrix: the equities of " + hand_class_name(i) + " vs " +
                               hand_class_name(j) + " and back do not add up to 1");
      }
    }
    if (row_total != std::uint64_t{kOpponentHands} * hand_class_num_combos(i)) {
      return fail(error, "equity matrix: wrong total weight for " + hand_class_name(i));
    }
  }
  return equity;
}

std::optional<Equity169> load_equity169(const std::string& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return fail(error, "cannot open " + path);
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
  if (file.bad()) return fail(error, "cannot read " + path);
  return equity169_from_bytes(bytes, error);
}

bool save_equity169(const Equity169& equity, const std::string& path, std::string* error) {
  const std::vector<std::uint8_t> bytes = equity169_to_bytes(equity);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

// ---- Part 4: evaluation and CFR+ ------------------------------------------------------------

PushFoldEvaluation evaluate_pushfold(const Equity169& equity, double stack_bb,
                                     const PushFoldStrategy& strategy) {
  return PushFoldGame(equity, stack_bb).evaluate(strategy);
}

// CFR+ (Tammelin 2014). Each player has one decision per hand class, and the game has no later
// decisions, so one CFR+ iteration is:
//
//   1. SB: compute its current strategy from its regrets (regret matching), the value of each
//      action against the BB's current strategy, and update its regrets.
//   2. BB: the same, against the SB's strategy after step 1 ("alternating updates").
//   3. Add each player's current strategy to its running average with weight t (iteration t
//      counts t times: "linear averaging", which forgets the poor early iterations faster).
//
// It is the average strategy, not the current one, that is guaranteed to approach a Nash
// equilibrium, so that is what is returned; every `check_every` iterations its exploitability is
// computed exactly and the loop stops once it is small enough.
std::optional<PushFoldSolution> solve_pushfold(const Equity169& equity, double stack_bb,
                                               const PushFoldOptions& options, std::string* error) {
  if (!std::isfinite(stack_bb) || stack_bb < 1 || stack_bb > kMaxStackBb) {
    return fail(error, "the stack must be from 1 to " +
                           std::to_string(static_cast<int>(kMaxStackBb)) + " big blinds");
  }
  if (!(options.target_exploitability > 0) || options.max_iterations < 1 ||
      options.check_every < 1) {
    return fail(error, "the target exploitability and the iteration counts must be positive");
  }
  if (equity.weights.size() != kMatrixCells || equity.half_pots.size() != kMatrixCells) {
    return fail(error, "the equity matrix is empty or has the wrong size");
  }

  const PushFoldGame game(equity, stack_bb);
  constexpr std::size_t n = kNumHandClasses;
  // Cumulative regrets (never negative in CFR+) of each class's two actions.
  std::vector<double> regret_shove(n, 0.0), regret_sb_fold(n, 0.0);
  std::vector<double> regret_call(n, 0.0), regret_bb_fold(n, 0.0);
  // The weighted sums of the strategies played, for the average strategy.
  std::vector<double> shove_sum(n, 0.0), call_sum(n, 0.0);
  double weight_sum = 0;

  std::vector<double> shove(n), call(n);                // current strategies
  std::vector<double> value_first(n), value_second(n);  // action values
  PushFoldSolution solution;
  solution.stack_bb = stack_bb;

  for (int t = 1; t <= options.max_iterations; ++t) {
    // 1. SB.
    for (std::size_t i = 0; i < n; ++i) {
      shove[i] = regret_matching(regret_shove[i], regret_sb_fold[i]);
    }
    for (std::size_t j = 0; j < n; ++j) {
      call[j] = regret_matching(regret_call[j], regret_bb_fold[j]);
    }
    game.sb_values(call, value_first, value_second);
    update_regrets(shove, value_first, value_second, regret_shove, regret_sb_fold);
    for (std::size_t i = 0; i < n; ++i) shove_sum[i] += t * shove[i];

    // 2. BB, against the SB's updated strategy.
    for (std::size_t i = 0; i < n; ++i) {
      shove[i] = regret_matching(regret_shove[i], regret_sb_fold[i]);
    }
    game.bb_values(shove, value_first, value_second);
    update_regrets(call, value_first, value_second, regret_call, regret_bb_fold);
    for (std::size_t j = 0; j < n; ++j) call_sum[j] += t * call[j];

    // 3. The average strategy, and whether it is good enough.
    weight_sum += t;
    if (t % options.check_every == 0 || t == options.max_iterations) {
      for (std::size_t i = 0; i < n; ++i) {
        solution.strategy.shove[i] = shove_sum[i] / weight_sum;
        solution.strategy.call[i] = call_sum[i] / weight_sum;
      }
      solution.evaluation = game.evaluate(solution.strategy);
      solution.iterations = t;
      solution.converged = solution.evaluation.exploitability <= options.target_exploitability;
      if (solution.converged) break;
    }
  }
  return solution;
}

}  // namespace nuts
