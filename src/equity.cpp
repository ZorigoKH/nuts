// Equity calculation. The file has three parts:
//
//   1. ShowdownTally: counts who wins each deal, shared by both methods.
//   2. Exact enumeration of every board completion (known hole cards).
//   3. Monte Carlo sampling (ranges).
#include "nuts/equity.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <system_error>
#include <thread>
#include <utility>

#include "nuts/evaluator.hpp"
#include "nuts/rng.hpp"

namespace nuts {
namespace {

constexpr int kBoardSize = 5;

// =============================================================================================
// Part 1: counting showdown results.
// =============================================================================================

// shares_[p][k] = number of deals in which player p was one of k players with the best hand
// (k = 1: p won alone; k >= 2: p split the pot k ways). Everything we report comes from these
// integer counts, so adding up the tallies of several threads is exact and the result does not
// depend on the order in which they are added.
//
// The counts live in a fixed-size array rather than a std::vector: each Monte Carlo thread keeps
// its tally in a local variable on its own stack, so two threads never write to the same cache
// line (heap blocks allocated by different threads can be neighbours in memory).
class ShowdownTally {
 public:
  explicit ShowdownTally(int num_players) : num_players_(num_players) {}

  // Records one showdown; ranks[p] is player p's HandRank (ranks.size() == number of players).
  void add(std::span<const HandRank> ranks) {
    HandRank best = 0;
    int num_best = 0;  // how many players have the best hand
    for (const HandRank rank : ranks) {
      if (rank > best) {
        best = rank;
        num_best = 1;
      } else if (rank == best) {
        ++num_best;
      }
    }
    for (std::size_t p = 0; p < ranks.size(); ++p) {
      if (ranks[p] == best) ++shares_[p][static_cast<std::size_t>(num_best)];
    }
    ++deals_;
  }

  void merge(const ShowdownTally& other) {
    for (std::size_t p = 0; p < shares_.size(); ++p) {
      for (std::size_t k = 0; k < shares_[p].size(); ++k) shares_[p][k] += other.shares_[p][k];
    }
    deals_ += other.deals_;
  }

  std::uint64_t deals() const { return deals_; }

  // Win, tie and equity of one player and, if estimate_error is set, the standard error of the
  // equity (for Monte Carlo, where the deals are random samples).
  PlayerEquity player_equity(int player, bool estimate_error) const {
    const auto& shares = shares_[static_cast<std::size_t>(player)];
    // In each deal the player gets a share X of the pot: 1 (won), 1/k (split k ways) or 0.
    // Equity is the average of X; sum_x and sum_x2 are the sums of X and X^2 over all deals.
    double sum_x = 0;
    double sum_x2 = 0;
    std::uint64_t ties = 0;
    for (int k = 1; k <= num_players_; ++k) {
      const auto count = static_cast<double>(shares[static_cast<std::size_t>(k)]);
      sum_x += count / k;
      sum_x2 += count / (static_cast<double>(k) * k);
      if (k >= 2) ties += shares[static_cast<std::size_t>(k)];
    }
    const auto n = static_cast<double>(deals_);
    PlayerEquity result;
    if (deals_ == 0) return result;
    result.win = static_cast<double>(shares[1]) / n;
    result.tie = static_cast<double>(ties) / n;
    result.equity = sum_x / n;
    if (estimate_error && deals_ > 1) {
      // Sample variance of X (dividing by n - 1), then the standard error of the mean.
      const double variance = (sum_x2 - sum_x * sum_x / n) / (n - 1);
      result.std_error = std::sqrt(std::max(variance, 0.0) / n);
    }
    return result;
  }

 private:
  int num_players_;
  std::uint64_t deals_ = 0;
  std::array<std::array<std::uint64_t, kMaxPlayers + 1>, kMaxPlayers> shares_{};
};

// =============================================================================================
// Input checks shared by both methods.
// =============================================================================================

// Stores the message (if error is not null); returns std::nullopt so callers can write
// `return fail(error, "...");`.
std::nullopt_t fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return std::nullopt;
}

// True if 2 <= num_players <= kMaxPlayers; otherwise stores a message and returns false.
bool check_num_players(std::size_t num_players, std::string* error) {
  if (num_players >= 2 && num_players <= kMaxPlayers) return true;
  if (error) {
    *error =
        "need 2 to " + std::to_string(kMaxPlayers) + " players, got " + std::to_string(num_players);
  }
  return false;
}

// The mask of the known board cards, or std::nullopt if there are more than 5, or an invalid or
// repeated card.
std::optional<std::uint64_t> check_board(std::span<const Card> board, std::string* error) {
  if (board.size() > kBoardSize) {
    return fail(error, "a board has at most 5 cards, got " + std::to_string(board.size()));
  }
  std::uint64_t mask = 0;
  for (const Card card : board) {
    if (!is_valid_card(card)) return fail(error, "invalid card id " + std::to_string(card));
    if (mask & card_bit(card)) return fail(error, to_string(card) + " is on the board twice");
    mask |= card_bit(card);
  }
  return mask;
}

// =============================================================================================
// Part 2: exact enumeration.
// =============================================================================================

// Calls visit(board) once for every way to add `Missing` more cards, taken from deck[first...],
// to the cards already in the mask `board`. Each level of the recursion adds one card to the mask
// it was given, so every board is built incrementally from the one above it. Missing is a
// compile-time constant, so the compiler turns this into Missing nested loops.
template <int Missing, typename Visit>
void for_each_board(std::span<const Card> deck, std::size_t first, std::uint64_t board,
                    Visit& visit) {
  if constexpr (Missing == 0) {
    visit(board);
  } else {
    // The cards are picked in increasing deck position, so each set of cards comes up once; stop
    // early enough to leave room for the Missing - 1 cards that follow this one.
    for (std::size_t i = first; i + Missing <= deck.size(); ++i) {
      for_each_board<Missing - 1>(deck, i + 1, board | card_bit(deck[i]), visit);
    }
  }
}

// =============================================================================================
// Part 3: Monte Carlo.
// =============================================================================================

// If a single deal has to start over this many times in a row because two players' combos share
// a card, we give up: the ranges can (practically) not be dealt together.
constexpr std::uint64_t kMaxDealAttempts = 1'000'000;

// A range ready for sampling: combos with weight 0 or that use a board card are left out.
struct SamplingRange {
  std::vector<std::uint64_t> masks;  // each combo's two cards as a card mask
  // cumulative[i] = total weight of combos 0..i, each weight divided by the largest one. Left
  // empty when all weights are equal, in which case a combo is simply picked uniformly.
  std::vector<double> cumulative;
  // True if the range is every combo that avoids the board, each once and with the same weight
  // ("random", "any two cards"). Such a player is dealt two cards from the deck left over after
  // the other players (see run_trials()), and `masks` is not used.
  bool any_two_cards = false;
};

// Everything the trials need that stays the same from trial to trial.
struct MonteCarloSetup {
  std::vector<SamplingRange> ranges;           // one per player
  std::vector<std::size_t> range_players;      // the players dealt from their ranges ...
  std::vector<std::size_t> any_two_players;    // ... and those dealt any two cards
  std::uint64_t board = 0;                     // mask of the known board cards
  int missing = 0;                             // number of board cards still to deal
  std::array<Card, kNumCards> deck{};          // the cards not on the known board ...
  std::size_t deck_size = 0;                   // ... are deck[0 .. deck_size - 1]
};

// True if `masks` holds each of the C(deck_size, 2) two-card combos of the deck exactly once.
// (The masks are already known to be two different cards off the board.)
bool is_every_combo(const std::vector<std::uint64_t>& masks, std::size_t deck_size) {
  if (masks.size() != deck_size * (deck_size - 1) / 2) return false;
  std::vector<std::uint64_t> sorted = masks;
  std::sort(sorted.begin(), sorted.end());
  return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();  // no repeats
}

// One combo from the range, picked with probability proportional to its weight.
std::uint64_t sample_combo(const SamplingRange& range, Xoshiro256StarStar& rng) {
  const auto size = static_cast<std::uint32_t>(range.masks.size());
  if (range.cumulative.empty()) return range.masks[rng.below(size)];
  // Lay the weights end to end on [0, total weight) and see where a uniformly random point lands
  // (inverse transform sampling): the first combo whose running total is above the point.
  const double point = rng.uniform01() * range.cumulative.back();
  const auto it = std::upper_bound(range.cumulative.begin(), range.cumulative.end(), point);
  // min(): if rounding put the point exactly at the total, take the last combo.
  const auto index =
      std::min<std::size_t>(static_cast<std::size_t>(it - range.cumulative.begin()), size - 1);
  return range.masks[index];
}

// Runs `trials` trials using the random stream `rng` and returns their tally, or std::nullopt if
// some deal needed kMaxDealAttempts attempts. Everything it writes to is local to the call, so
// several calls can run in parallel threads.
std::optional<ShowdownTally> run_trials(const MonteCarloSetup& setup, std::uint64_t trials,
                                        Xoshiro256StarStar rng) {
  const std::size_t num_players = setup.ranges.size();
  ShowdownTally tally(static_cast<int>(num_players));
  std::array<Card, kNumCards> deck = setup.deck;  // this thread's copy, shuffled in place
  std::array<std::uint64_t, kMaxPlayers> hole{};  // each player's hole cards (a mask)
  std::array<HandRank, kMaxPlayers> ranks{};

  for (std::uint64_t trial = 0; trial < trials; ++trial) {
    // 1. Hole cards of the players dealt from their ranges. Pick a combo for each in turn. If it
    //    shares a card with a combo already picked, throw these picks away and start again from
    //    the first of them (rejection sampling). This gives every combination of combos without
    //    shared cards a probability proportional to the product of their weights, which is what
    //    "players with these ranges" means. (Re-picking only the clashing player would favour
    //    some combinations over others.)
    std::uint64_t used = 0;  // cards in the hands picked so far
    std::uint64_t attempts = 0;
    for (std::size_t k = 0; k < setup.range_players.size();) {
      const std::size_t p = setup.range_players[k];
      const std::uint64_t combo = sample_combo(setup.ranges[p], rng);
      if (combo & used) {
        if (++attempts == kMaxDealAttempts) return std::nullopt;
        used = 0;
        k = 0;
        continue;
      }
      hole[p] = combo;
      used |= combo;
      ++k;
    }

    // 2. All other cards come from a partial Fisher-Yates shuffle of the deck: position i gets a
    //    random card from positions i..end. Cards already in someone's hand are skipped. The
    //    cards not skipped are a uniformly random sequence of the cards nobody holds yet. (The
    //    deck is not put back in order between trials; shuffling from any order is equally
    //    random.) There are always enough cards: 2 * 23 hole cards + 5 board cards <= 52.
    std::size_t i = 0;
    const auto next_card = [&]() -> std::uint64_t {
      while (true) {
        const std::size_t j = i + rng.below(static_cast<std::uint32_t>(setup.deck_size - i));
        std::swap(deck[i], deck[j]);
        const std::uint64_t card = card_bit(deck[i++]);
        if (!(card & used)) return card;
      }
    };

    //    First the players with any two cards. This gives the same result as dealing them by
    //    rejection sampling too: the number of ways to deal them from the cards left is the
    //    same whatever the other players hold, so rejection would not favour any deal of the
    //    others, and would give them two cards uniformly at random from the cards left, as
    //    here. Unlike rejection, it never fails, however many such players there are.
    for (const std::size_t p : setup.any_two_players) {
      const std::uint64_t first = next_card();
      hole[p] = first | next_card();
    }
    //    Then the rest of the board.
    std::uint64_t board = setup.board;
    for (int k = 0; k < setup.missing; ++k) board |= next_card();

    // 3. Showdown.
    for (std::size_t p = 0; p < num_players; ++p) ranks[p] = evaluate(board | hole[p]);
    tally.add(std::span<const HandRank>(ranks.data(), num_players));
  }
  return tally;
}

}  // namespace

std::optional<EquityResult> equity_exact(std::span<const HoleCards> hands,
                                         std::span<const Card> board, std::string* error) {
  if (!check_num_players(hands.size(), error)) return std::nullopt;
  const std::optional<std::uint64_t> board_mask = check_board(board, error);
  if (!board_mask) return std::nullopt;

  // Each player's cards as a mask; `dead` collects every card seen so far to catch repeats.
  std::array<std::uint64_t, kMaxPlayers> hand_masks{};
  std::uint64_t dead = *board_mask;
  for (std::size_t p = 0; p < hands.size(); ++p) {
    for (const Card card : hands[p]) {
      if (!is_valid_card(card)) return fail(error, "invalid card id " + std::to_string(card));
      if (dead & card_bit(card)) return fail(error, to_string(card) + " is used twice");
      dead |= card_bit(card);
      hand_masks[p] |= card_bit(card);
    }
  }

  // The cards that can still come on the board.
  std::vector<Card> deck;
  for (int id = 0; id < kNumCards; ++id) {
    if (!(dead & card_bit(static_cast<Card>(id)))) deck.push_back(static_cast<Card>(id));
  }

  const std::size_t num_players = hands.size();
  ShowdownTally tally(static_cast<int>(num_players));
  std::array<HandRank, kMaxPlayers> ranks{};
  auto showdown = [&](std::uint64_t full_board) {
    for (std::size_t p = 0; p < num_players; ++p) ranks[p] = evaluate(full_board | hand_masks[p]);
    tally.add(std::span<const HandRank>(ranks.data(), num_players));
  };
  switch (kBoardSize - static_cast<int>(board.size())) {
    case 0:
      for_each_board<0>(deck, 0, *board_mask, showdown);
      break;
    case 1:
      for_each_board<1>(deck, 0, *board_mask, showdown);
      break;
    case 2:
      for_each_board<2>(deck, 0, *board_mask, showdown);
      break;
    case 3:
      for_each_board<3>(deck, 0, *board_mask, showdown);
      break;
    case 4:
      for_each_board<4>(deck, 0, *board_mask, showdown);
      break;
    case 5:
      for_each_board<5>(deck, 0, *board_mask, showdown);
      break;
  }

  EquityResult result;
  result.exact = true;
  result.threads = 1;
  result.deals = tally.deals();
  for (std::size_t p = 0; p < num_players; ++p) {
    result.players.push_back(tally.player_equity(static_cast<int>(p), /*estimate_error=*/false));
  }
  return result;
}

std::optional<EquityResult> equity_monte_carlo(std::span<const Range> ranges,
                                               std::span<const Card> board, std::uint64_t trials,
                                               std::uint64_t seed, int threads,
                                               std::string* error) {
  if (!check_num_players(ranges.size(), error)) return std::nullopt;
  if (trials == 0) return fail(error, "need at least 1 trial");
  const std::optional<std::uint64_t> board_mask = check_board(board, error);
  if (!board_mask) return std::nullopt;

  MonteCarloSetup setup;
  setup.board = *board_mask;
  setup.missing = kBoardSize - static_cast<int>(board.size());
  for (int id = 0; id < kNumCards; ++id) {
    const auto card = static_cast<Card>(id);
    if (!(setup.board & card_bit(card))) setup.deck[setup.deck_size++] = card;
  }

  for (std::size_t p = 0; p < ranges.size(); ++p) {
    const std::string player = "player " + std::to_string(p + 1);
    SamplingRange sampling;
    std::vector<double> weights;  // of the combos kept in sampling.masks
    for (const Combo& combo : ranges[p]) {
      const auto [a, b] = combo.cards;
      if (!is_valid_card(a) || !is_valid_card(b) || a == b) {
        return fail(error, player + "'s range has a combo with an invalid or repeated card");
      }
      if (!std::isfinite(combo.weight) || combo.weight < 0) {
        return fail(error, player + "'s range has a negative or non-finite weight");
      }
      const std::uint64_t mask = card_bit(a) | card_bit(b);
      if (combo.weight == 0 || (mask & setup.board)) continue;  // can never be dealt
      sampling.masks.push_back(mask);
      weights.push_back(combo.weight);
    }
    if (sampling.masks.empty()) {
      return fail(error, player + "'s range has no combo left that avoids the board cards");
    }
    const double max_weight = *std::max_element(weights.begin(), weights.end());
    const bool equal_weights =
        std::all_of(weights.begin(), weights.end(), [&](double w) { return w == max_weight; });
    if (!equal_weights) {
      // Running totals of the weights divided by the largest one. Every term is at most 1, so
      // the total cannot overflow to infinity, however large the weights are.
      double total = 0;
      for (const double w : weights) {
        total += w / max_weight;
        sampling.cumulative.push_back(total);
      }
    }
    sampling.any_two_cards = equal_weights && is_every_combo(sampling.masks, setup.deck_size);
    (sampling.any_two_cards ? setup.any_two_players : setup.range_players).push_back(p);
    setup.ranges.push_back(std::move(sampling));
  }

  // Threads: as many as asked for (0 or less = one per hardware thread), but not more than
  // kMaxThreads or trials.
  if (threads > kMaxThreads) {
    return fail(error, "threads must be at most " + std::to_string(kMaxThreads) + ", got " +
                           std::to_string(threads));
  }
  std::uint64_t num_threads =
      threads > 0 ? static_cast<std::uint64_t>(threads)
                  : std::clamp(std::thread::hardware_concurrency(), 1u, unsigned{kMaxThreads});
  num_threads = std::min(num_threads, trials);

  // Thread t gets its own random stream, the generator seeded with `seed` and jumped t times,
  // and a fixed share of the trials (the first trials % num_threads threads do one more). So the
  // result depends only on (seed, num_threads), not on how the threads happen to be scheduled.
  std::vector<Xoshiro256StarStar> streams;
  Xoshiro256StarStar stream(seed);
  for (std::uint64_t t = 0; t < num_threads; ++t) {
    streams.push_back(stream);
    stream.jump();
  }
  const auto trials_for_thread = [&](std::uint64_t t) {
    return trials / num_threads + (t < trials % num_threads ? 1 : 0);
  };

  std::vector<std::optional<ShowdownTally>> tallies(num_threads);  // one slot per thread
  if (num_threads == 1) {
    tallies[0] = run_trials(setup, trials, streams[0]);
  } else {
    std::vector<std::thread> workers;
    bool started_all = true;
    for (std::uint64_t t = 0; t < num_threads; ++t) {
      // Starting a thread throws std::system_error if the operating system refuses. Catch it:
      // the threads already started must be joined before `workers` is destroyed (destroying a
      // std::thread that is still running ends the whole program).
      try {
        workers.emplace_back(
            [&, t] { tallies[t] = run_trials(setup, trials_for_thread(t), streams[t]); });
      } catch (const std::system_error&) {
        started_all = false;
        break;
      }
    }
    for (std::thread& worker : workers) worker.join();
    if (!started_all) {
      return fail(error, "could not start " + std::to_string(num_threads) + " threads");
    }
  }

  ShowdownTally total(static_cast<int>(ranges.size()));
  for (const std::optional<ShowdownTally>& tally : tallies) {
    if (!tally) {
      return fail(error, "could not deal the ranges without two players getting the same card: " +
                             std::to_string(kMaxDealAttempts) +
                             " deals in a row failed (the ranges overlap too much)");
    }
    total.merge(*tally);
  }

  EquityResult result;
  result.exact = false;
  result.threads = static_cast<int>(num_threads);
  result.deals = total.deals();
  for (std::size_t p = 0; p < ranges.size(); ++p) {
    result.players.push_back(total.player_equity(static_cast<int>(p), /*estimate_error=*/true));
  }
  return result;
}

}  // namespace nuts
