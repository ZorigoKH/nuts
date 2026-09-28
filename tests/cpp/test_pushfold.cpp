#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/equity.hpp"
#include "nuts/pushfold.hpp"
#include "nuts/range.hpp"

#ifndef NUTS_DATA_DIR
#error "NUTS_DATA_DIR must be defined (see CMakeLists.txt)"
#endif

namespace {

using Catch::Matchers::WithinAbs;
using nuts::Equity169;
using nuts::HoleCards;
using nuts::kNumHandClasses;
using nuts::kPreflopBoards;

const std::string kEquityFile = std::string(NUTS_DATA_DIR) + "/equity169.bin";

// The committed matrix, loaded once.
const Equity169& equity() {
  static const Equity169 matrix = [] {
    std::string error;
    std::optional<Equity169> loaded = nuts::load_equity169(kEquityFile, &error);
    INFO(error);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
  }();
  return matrix;
}

int hand_class(const char* name) {
  const std::optional<int> index = nuts::parse_hand_class(name);
  INFO(name);
  REQUIRE(index.has_value());
  return *index;
}

HoleCards hole_cards(const char* text) {
  const std::optional<HoleCards> cards = nuts::parse_hole_cards(text);
  REQUIRE(cards.has_value());
  return *cards;
}

// Half pots won by `a` against `b` over all C(48, 5) boards: 2 per win, 1 per tie.
std::uint64_t half_pots(const HoleCards& a, const HoleCards& b) {
  const std::vector<HoleCards> hands = {a, b};
  const std::optional<nuts::EquityResult> result = nuts::equity_exact(hands, {});
  REQUIRE(result.has_value());
  REQUIRE(result->deals == kPreflopBoards);
  const auto deals = static_cast<double>(result->deals);
  const auto wins = static_cast<std::uint64_t>(std::llround(result->players[0].win * deals));
  const auto ties = static_cast<std::uint64_t>(std::llround(result->players[0].tie * deals));
  return 2 * wins + ties;
}

bool share_a_card(const HoleCards& a, const HoleCards& b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

nuts::PushFoldSolution solve(double stack_bb) {
  std::string error;
  const std::optional<nuts::PushFoldSolution> solution =
      nuts::solve_pushfold(equity(), stack_bb, {}, &error);
  INFO(error);
  REQUIRE(solution.has_value());
  return *solution;
}

// The share of the 1,326 hands for which the strategy takes the action.
double share_of_hands(const std::vector<double>& probability) {
  double hands = 0;
  for (int c = 0; c < kNumHandClasses; ++c) {
    hands += probability[static_cast<std::size_t>(c)] * nuts::hand_class_num_combos(c);
  }
  return hands / nuts::kNumHoleCardCombos;
}

}  // namespace

// ---- Hand classes ---------------------------------------------------------------------------

TEST_CASE("hand classes: names and chart layout", "[pushfold]") {
  CHECK(nuts::hand_class_name(0) == "AA");
  CHECK(nuts::hand_class_name(1) == "AKs");    // row 0, column 1: above the diagonal
  CHECK(nuts::hand_class_name(13) == "AKo");   // row 1, column 0: below the diagonal
  CHECK(nuts::hand_class_name(12) == "A2s");   // end of the first row
  CHECK(nuts::hand_class_name(156) == "A2o");  // start of the last row
  CHECK(nuts::hand_class_name(163) == "72o");
  CHECK(nuts::hand_class_name(167) == "32o");
  CHECK(nuts::hand_class_name(168) == "22");

  std::set<std::string> names;
  int pairs = 0, suited = 0, offsuit = 0;
  for (int c = 0; c < kNumHandClasses; ++c) {
    const std::string name = nuts::hand_class_name(c);
    names.insert(name);
    CHECK(nuts::parse_hand_class(name) == c);
    if (name.size() == 2) ++pairs;
    if (name.size() == 3 && name[2] == 's') ++suited;
    if (name.size() == 3 && name[2] == 'o') ++offsuit;
  }
  CHECK(names.size() == 169);
  CHECK(pairs == 13);
  CHECK(suited == 78);
  CHECK(offsuit == 78);

  CHECK(nuts::parse_hand_class("aks") == 1);
  CHECK(nuts::parse_hand_class("KAs") == 1);
  CHECK(nuts::parse_hand_class("27O") == 163);
  CHECK_FALSE(nuts::parse_hand_class("AK").has_value());  // needs s or o
  CHECK_FALSE(nuts::parse_hand_class("AAs").has_value());
  CHECK_FALSE(nuts::parse_hand_class("AKx").has_value());
  CHECK_FALSE(nuts::parse_hand_class("A").has_value());
  CHECK_FALSE(nuts::parse_hand_class("AKs+").has_value());
}

TEST_CASE("hand classes: every two-card hand is in exactly one class", "[pushfold]") {
  std::set<HoleCards> seen;
  for (int c = 0; c < kNumHandClasses; ++c) {
    const std::vector<HoleCards> combos = nuts::hand_class_combos(c);
    CHECK(static_cast<int>(combos.size()) == nuts::hand_class_num_combos(c));
    for (const HoleCards& cards : combos) {
      CHECK(nuts::hand_class_of(cards) == c);
      CHECK(seen.insert(cards).second);
    }
  }
  CHECK(seen.size() == nuts::kNumHoleCardCombos);
  CHECK(nuts::hand_class_of(hole_cards("7h2c")) == hand_class("72o"));
  CHECK(nuts::hand_class_of(hole_cards("2c7c")) == hand_class("72s"));
  CHECK(nuts::hand_class_of(hole_cards("KdKs")) == hand_class("KK"));
}

// ---- Suit symmetry --------------------------------------------------------------------------

TEST_CASE("suit symmetry: canonical matchups", "[pushfold]") {
  using nuts::detail::canonical_matchup;
  // Renaming hearts <-> spades turns one matchup into the other.
  const HoleCards ak_hearts = hole_cards("AhKh");
  const HoleCards ak_spades = hole_cards("AsKs");
  const HoleCards qq_sd = hole_cards("QsQd");
  const HoleCards qq_hd = hole_cards("QhQd");
  CHECK(canonical_matchup(ak_hearts, qq_sd).key == canonical_matchup(ak_spades, qq_hd).key);
  // Swapping the players gives the same key, with `swapped` flipped.
  const auto forward = canonical_matchup(ak_hearts, qq_sd);
  const auto backward = canonical_matchup(qq_sd, ak_hearts);
  CHECK(forward.key == backward.key);
  CHECK(forward.swapped != backward.swapped);
  // AKs against AKo is not the same matchup as AKs against AKs.
  CHECK(canonical_matchup(ak_hearts, hole_cards("AsKd")).key !=
        canonical_matchup(ak_hearts, ak_spades).key);

  // Decoding a key gives a matchup in canonical form.
  const auto decoded = nuts::detail::decode_matchup(forward.key);
  const auto again = canonical_matchup(decoded[0], decoded[1]);
  CHECK(again.key == forward.key);
  CHECK_FALSE(again.swapped);

  // The canonical form has the same result as the original (seen from the right player).
  const std::uint64_t original = half_pots(ak_hearts, qq_sd);
  const std::uint64_t canonical =
      forward.swapped ? 2 * std::uint64_t{kPreflopBoards} - half_pots(decoded[0], decoded[1])
                      : half_pots(decoded[0], decoded[1]);
  CHECK(original == canonical);
}

TEST_CASE("suit symmetry: 47,008 distinct heads-up matchups", "[pushfold]") {
  // All 1,624,350 ordered pairs of hands without a common card fall into 47,008 matchups up to
  // suit renaming and player order (tools/verify/count_matchups.py counts them by brute force,
  // independently of this code).
  std::vector<HoleCards> hands;
  for (int c = 0; c < kNumHandClasses; ++c) {
    for (const HoleCards& cards : nuts::hand_class_combos(c)) hands.push_back(cards);
  }
  std::vector<std::uint32_t> keys;
  for (const HoleCards& a : hands) {
    for (const HoleCards& b : hands) {
      if (!share_a_card(a, b)) keys.push_back(nuts::detail::canonical_matchup(a, b).key);
    }
  }
  CHECK(keys.size() == nuts::kPreflopMatchups);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  CHECK(keys.size() == 47'008);
}

// ---- The equity matrix ----------------------------------------------------------------------

TEST_CASE("equity matrix: weights count the hand pairs without a common card", "[pushfold]") {
  const Equity169& e = equity();
  // Recount every weight directly from the hands.
  std::vector<std::uint32_t> counted(static_cast<std::size_t>(kNumHandClasses) * kNumHandClasses);
  for (int i = 0; i < kNumHandClasses; ++i) {
    for (const HoleCards& a : nuts::hand_class_combos(i)) {
      for (int j = 0; j < kNumHandClasses; ++j) {
        for (const HoleCards& b : nuts::hand_class_combos(j)) {
          if (!share_a_card(a, b)) ++counted[Equity169::index(i, j)];
        }
      }
    }
  }
  CHECK(e.weights == counted);

  std::uint64_t total = 0;
  for (int i = 0; i < kNumHandClasses; ++i) {
    std::uint64_t row = 0;
    for (int j = 0; j < kNumHandClasses; ++j) {
      row += e.weight(i, j);
      CHECK(e.weight(i, j) == e.weight(j, i));
    }
    // Each hand meets each of the C(50, 2) = 1,225 hands without a common card once.
    CHECK(row == 1'225u * static_cast<std::uint64_t>(nuts::hand_class_num_combos(i)));
    total += row;
  }
  CHECK(total == nuts::kPreflopMatchups);  // 1,326 * 1,225 = 1,624,350

  CHECK(e.weight(hand_class("AA"), hand_class("KK")) == 36);   // no common rank: 6 * 6
  CHECK(e.weight(hand_class("AA"), hand_class("AA")) == 6);    // only the other two aces
  CHECK(e.weight(hand_class("AA"), hand_class("AKs")) == 12);  // 2 of the 4 AKs per AA
  CHECK(e.weight(hand_class("AKo"), hand_class("72o")) == 144);
}

TEST_CASE("equity matrix: equities are consistent", "[pushfold]") {
  const Equity169& e = equity();
  for (int i = 0; i < kNumHandClasses; ++i) {
    for (int j = 0; j < kNumHandClasses; ++j) {
      // i against j and j against i are the same hand pairs seen from both sides.
      const std::uint64_t both = std::uint64_t{e.half_pots_won(i, j)} + e.half_pots_won(j, i);
      CHECK(both == 2 * std::uint64_t{kPreflopBoards} * e.weight(i, j));
      CHECK_THAT(e.equity(i, j) + e.equity(j, i), WithinAbs(1.0, 1e-12));
      CHECK(e.equity(i, j) >= 0);
      CHECK(e.equity(i, j) <= 1);
    }
    CHECK(e.equity(i, i) == 0.5);  // a class against itself
    if (i != hand_class("AA")) CHECK(e.equity(hand_class("AA"), i) > 0.5);
  }
  // Some well-known values.
  CHECK_THAT(e.equity(hand_class("AA"), hand_class("KK")), WithinAbs(0.82, 0.01));
  CHECK_THAT(e.equity(hand_class("AKs"), hand_class("QQ")), WithinAbs(0.46, 0.01));
  CHECK_THAT(e.equity(hand_class("AKo"), hand_class("22")), WithinAbs(0.47, 0.02));
}

TEST_CASE("equity matrix: entries recomputed a different way", "[pushfold]") {
  // Every hand of class i is a suit renaming of any fixed hand `a` of that class, and renaming
  // the suits maps class j onto itself. So the total over (i, j) equals num_combos(i) times the
  // total of the single hand `a` against all hands of class j it shares no card with. This
  // recomputes the entry without the canonical-matchup machinery of compute_equity169().
  const Equity169& e = equity();
  // (Each cell takes 6 exact enumerations here. tools/verify/eval7_crosscheck.py recomputes these
  // two cells, and 72o vs 32o, with the eval7 library and checks the same integers.)
  const std::vector<std::pair<const char*, const char*>> cells = {
      {"AA", "KK"},    // pairs, no common rank
      {"AKs", "AKo"},  // common ranks: card removal
  };
  for (const auto& [first, second] : cells) {
    INFO(first << " vs " << second);
    const int i = hand_class(first);
    const int j = hand_class(second);
    const HoleCards a = nuts::hand_class_combos(i)[0];
    std::uint64_t weight = 0;
    std::uint64_t total = 0;
    for (const HoleCards& b : nuts::hand_class_combos(j)) {
      if (share_a_card(a, b)) continue;
      ++weight;
      total += half_pots(a, b);
    }
    const auto num_a = static_cast<std::uint64_t>(nuts::hand_class_num_combos(i));
    CHECK(e.weight(i, j) == num_a * weight);
    CHECK(e.half_pots_won(i, j) == num_a * total);
  }
}

TEST_CASE("equity matrix: file format", "[pushfold]") {
  const std::vector<std::uint8_t> bytes = nuts::equity169_to_bytes(equity());
  CHECK(bytes.size() == 228'504);  // 16-byte header + 2 * 169 * 169 * 4
  CHECK(bytes == read_file(kEquityFile));
  const std::optional<Equity169> round_trip = nuts::equity169_from_bytes(bytes);
  REQUIRE(round_trip.has_value());
  CHECK(round_trip->weights == equity().weights);
  CHECK(round_trip->half_pots == equity().half_pots);

  // Damaged files are rejected with a message.
  const auto rejected = [](std::vector<std::uint8_t> damaged) {
    std::string error;
    const bool ok = nuts::equity169_from_bytes(damaged, &error).has_value();
    INFO(error);
    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
  };
  std::vector<std::uint8_t> copy = bytes;
  copy[0] = 'X';  // magic
  rejected(copy);
  copy = bytes;
  copy[8] = 170;  // number of classes
  rejected(copy);
  copy = bytes;
  copy.pop_back();  // too short
  rejected(copy);
  copy = bytes;
  copy[16 + 4 * 1] += 1;  // weight(AA, AKs): no longer symmetric
  rejected(copy);
  copy = bytes;
  copy[16 + 2 * 169 * 169 + 4 * 5] ^= 1;  // a half-pot count: equities no longer add up to 1
  rejected(copy);

  std::string error;
  CHECK_FALSE(nuts::load_equity169(kEquityFile + ".missing", &error).has_value());
  CHECK_FALSE(error.empty());
}

// ---- The push/fold game ---------------------------------------------------------------------

TEST_CASE("push/fold: values of simple strategies", "[pushfold]") {
  const double stack = 10;
  nuts::PushFoldStrategy strategy;  // all zeros: SB always folds, BB never calls

  nuts::PushFoldEvaluation e = nuts::evaluate_pushfold(equity(), stack, strategy);
  CHECK_THAT(e.sb_value, WithinAbs(-0.5, 1e-12));
  // Against a BB that never calls, the SB's best response shoves everything and wins 1.
  CHECK_THAT(e.sb_best_response, WithinAbs(1.0, 1e-12));
  // The BB gets the 0.5 whatever it does.
  CHECK_THAT(e.bb_best_response, WithinAbs(0.5, 1e-12));
  CHECK_THAT(e.exploitability, WithinAbs((1.5 + 0.0) / 2, 1e-12));

  // SB always shoves, BB always folds: the SB wins 1 every hand.
  std::fill(strategy.shove.begin(), strategy.shove.end(), 1.0);
  CHECK_THAT(nuts::evaluate_pushfold(equity(), stack, strategy).sb_value, WithinAbs(1.0, 1e-12));

  // SB always shoves, BB always calls: every hand is all-in, and over all deals the SB's average
  // equity is exactly 1/2 (each pair of hands shows up from both sides), so its value is 0.
  std::fill(strategy.call.begin(), strategy.call.end(), 1.0);
  e = nuts::evaluate_pushfold(equity(), stack, strategy);
  CHECK_THAT(e.sb_value, WithinAbs(0.0, 1e-12));
  CHECK(e.exploitability > 0.1);  // far from an equilibrium: e.g. the BB calls 10 bb with 72o
}

TEST_CASE("push/fold: CFR+ finds an equilibrium", "[pushfold]") {
  double previous_shove = 1;
  double previous_call = 1;
  for (const double stack : {1.0, 2.0, 5.0, 10.0, 15.0, 20.0}) {
    INFO("stack " << stack);
    const nuts::PushFoldSolution solution = solve(stack);
    const nuts::PushFoldStrategy& s = solution.strategy;
    INFO("exploitability " << solution.evaluation.exploitability << " after " << solution.iterations
                           << " iterations");
    CHECK(solution.evaluation.exploitability < 1e-3);  // the requirement (big blinds per hand)
    CHECK(solution.evaluation.exploitability <= nuts::PushFoldOptions{}.target_exploitability);
    CHECK(solution.evaluation.exploitability >= -1e-12);

    // The reported evaluation is that of the returned strategy.
    const nuts::PushFoldEvaluation again = nuts::evaluate_pushfold(equity(), stack, s);
    CHECK(again.exploitability == solution.evaluation.exploitability);

    // Sanity: AA always shoves and always calls; 72o folds both ways at 20 bb.
    CHECK(s.shove[static_cast<std::size_t>(hand_class("AA"))] > 0.999);
    CHECK(s.call[static_cast<std::size_t>(hand_class("AA"))] > 0.999);
    if (stack == 20.0) {
      CHECK(s.shove[static_cast<std::size_t>(hand_class("72o"))] < 0.001);
      CHECK(s.call[static_cast<std::size_t>(hand_class("72o"))] < 0.001);
    }

    // Deeper stacks risk more, so both players play fewer hands. This holds between stacks as
    // far apart as these, not for every small step: the SB's range is 61.86% of hands at 8 bb
    // and 61.96% at 8.5 bb (data/pushfold.json). At 1 and 2 bb the BB calls with everything, and
    // the averaged strategies are then 1 only up to about 1e-6, hence the small tolerance.
    const double shove = share_of_hands(s.shove);
    const double call = share_of_hands(s.call);
    CHECK(shove <= previous_shove + 1e-4);
    CHECK(call <= previous_call + 1e-4);
    previous_shove = shove;
    previous_call = call;
  }
}

TEST_CASE("push/fold: invalid input", "[pushfold]") {
  std::string error;
  CHECK_FALSE(nuts::solve_pushfold(equity(), 0.5, {}, &error).has_value());
  CHECK_FALSE(error.empty());
  CHECK_FALSE(nuts::solve_pushfold(equity(), std::nan(""), {}).has_value());
  // Deep stacks: above kMaxStackBb is rejected (the solve would take seconds to minutes).
  CHECK_FALSE(nuts::solve_pushfold(equity(), nuts::kMaxStackBb + 0.5, {}).has_value());
  CHECK_FALSE(nuts::solve_pushfold(equity(), 1e9, {}).has_value());
  nuts::PushFoldOptions options;
  options.max_iterations = 0;
  CHECK_FALSE(nuts::solve_pushfold(equity(), 10, options).has_value());
  CHECK_FALSE(nuts::solve_pushfold(Equity169{}, 10, {}).has_value());
}

TEST_CASE("push/fold: convergence is reported", "[pushfold]") {
  // The largest stack allowed converges with the default options ...
  const nuts::PushFoldSolution deepest = solve(nuts::kMaxStackBb);
  CHECK(deepest.converged);
  CHECK(deepest.evaluation.exploitability <= nuts::PushFoldOptions{}.target_exploitability);

  // ... and a solve cut short says so, instead of passing for an equilibrium.
  nuts::PushFoldOptions options;
  options.max_iterations = 10;
  options.check_every = 5;
  const std::optional<nuts::PushFoldSolution> short_run =
      nuts::solve_pushfold(equity(), 10, options);
  REQUIRE(short_run.has_value());
  CHECK(short_run->iterations == 10);
  CHECK_FALSE(short_run->converged);
  CHECK(short_run->evaluation.exploitability > options.target_exploitability);
}
