#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/equity.hpp"
#include "nuts/range.hpp"

namespace {

using Catch::Matchers::WithinAbs;
using nuts::EquityResult;
using nuts::HoleCards;
using nuts::Range;

std::vector<HoleCards> hands(std::initializer_list<const char*> texts) {
  std::vector<HoleCards> out;
  for (const char* text : texts) {
    const std::optional<HoleCards> cards = nuts::parse_hole_cards(text);
    REQUIRE(cards.has_value());
    out.push_back(*cards);
  }
  return out;
}

std::vector<nuts::Card> board(const char* text) {
  const auto cards = nuts::parse_cards(text);
  REQUIRE(cards.has_value());
  return *cards;
}

std::vector<Range> ranges(std::initializer_list<const char*> texts) {
  std::vector<Range> out;
  for (const char* text : texts) {
    const std::optional<Range> range = nuts::parse_range(text);
    REQUIRE(range.has_value());
    out.push_back(*range);
  }
  return out;
}

EquityResult exact(std::initializer_list<const char*> hole_cards, const char* board_text = "") {
  std::string error;
  const std::optional<EquityResult> result =
      nuts::equity_exact(hands(hole_cards), board(board_text), &error);
  INFO(error);
  REQUIRE(result.has_value());
  return *result;
}

EquityResult monte_carlo(std::initializer_list<const char*> range_texts, const char* board_text,
                         std::uint64_t trials, std::uint64_t seed, int threads) {
  std::string error;
  const std::optional<EquityResult> result = nuts::equity_monte_carlo(
      ranges(range_texts), board(board_text), trials, seed, threads, &error);
  INFO(error);
  REQUIRE(result.has_value());
  return *result;
}

// The equities of all players add up to 1 (the whole pot always goes to someone).
double total_equity(const EquityResult& result) {
  double total = 0;
  for (const nuts::PlayerEquity& player : result.players) total += player.equity;
  return total;
}

// Checks a Monte Carlo estimate against the true value: within 4 standard errors. (A correct
// estimate is further away than that about once in 16,000 tries; the seeds are fixed, so the
// test is deterministic anyway.)
void check_within_4_se(const nuts::PlayerEquity& estimate, double truth) {
  INFO("estimate " << estimate.equity << " +- " << estimate.std_error << ", truth " << truth);
  CHECK(estimate.std_error > 0);
  CHECK(std::abs(estimate.equity - truth) < 4 * estimate.std_error);
}

// The equity of every player when the players have ranges, computed exactly: the average of
// equity_exact() over all combinations of combos that share no card and avoid the board,
// weighted by the product of the combo weights. This is the definition that Monte Carlo
// estimates. Only for small cases (two players, flop or later).
std::vector<double> exact_range_equity(const std::vector<Range>& player_ranges,
                                       const std::vector<nuts::Card>& board_cards) {
  REQUIRE(player_ranges.size() == 2);
  const std::uint64_t board_mask = nuts::mask_of(board_cards);
  std::vector<double> weighted_sum(2, 0.0);
  double total_weight = 0;
  for (const nuts::Combo& a : player_ranges[0]) {
    for (const nuts::Combo& b : player_ranges[1]) {
      const std::uint64_t mask_a = nuts::mask_of(a.cards);
      const std::uint64_t mask_b = nuts::mask_of(b.cards);
      if ((mask_a & mask_b) || (mask_a & board_mask) || (mask_b & board_mask)) continue;
      const std::vector<HoleCards> pair = {a.cards, b.cards};
      const std::optional<EquityResult> result = nuts::equity_exact(pair, board_cards);
      REQUIRE(result.has_value());
      const double weight = a.weight * b.weight;
      weighted_sum[0] += weight * result->players[0].equity;
      weighted_sum[1] += weight * result->players[1].equity;
      total_weight += weight;
    }
  }
  REQUIRE(total_weight > 0);
  return {weighted_sum[0] / total_weight, weighted_sum[1] / total_weight};
}

}  // namespace

// ---- Exact enumeration ----------------------------------------------------------------------

// Expected values: a brute force with eval7, a different poker library (every board, exact
// fractions), gives exactly these doubles; see tools/verify/eval7_crosscheck.py, which checks
// them. tests/python/test_eval7.py runs the same kind of brute force on boards with 3+ cards.
TEST_CASE("exact equity: known heads-up preflop matchups", "[equity]") {
  const EquityResult aa_kk = exact({"AhAs", "KdKc"});
  CHECK(aa_kk.exact);
  CHECK(aa_kk.deals == 1'712'304);  // C(48, 5)
  CHECK_THAT(aa_kk.players[0].win, WithinAbs(0.81064577318046327, 1e-12));
  CHECK_THAT(aa_kk.players[0].tie, WithinAbs(0.0038182472271278932, 1e-12));
  CHECK_THAT(aa_kk.players[0].equity, WithinAbs(0.81255489679402726, 1e-12));
  CHECK_THAT(aa_kk.players[1].win, WithinAbs(0.18553597959240883, 1e-12));
  CHECK_THAT(aa_kk.players[1].equity, WithinAbs(0.18744510320597277, 1e-12));
  CHECK(aa_kk.players[0].std_error == 0);

  // The classic "coin flip": suited overcards against a small pair.
  const EquityResult flip = exact({"AsKs", "2h2d"});
  CHECK_THAT(flip.players[0].equity, WithinAbs(0.50084243218493907, 1e-12));
  CHECK_THAT(flip.players[1].equity, WithinAbs(0.49915756781506088, 1e-12));
  CHECK_THAT(flip.players[0].tie, WithinAbs(0.0062926910174828767, 1e-12));
}

TEST_CASE("exact equity: three players and known boards", "[equity]") {
  const EquityResult preflop = exact({"Ac2d", "7h7s", "KsQs"});
  CHECK(preflop.deals == 1'370'754);  // C(46, 5)
  CHECK_THAT(preflop.players[0].equity, WithinAbs(0.24679142525451928, 1e-12));
  CHECK_THAT(preflop.players[1].equity, WithinAbs(0.37308031443521839, 1e-12));
  CHECK_THAT(preflop.players[2].equity, WithinAbs(0.38012826031026231, 1e-12));

  const EquityResult flop = exact({"AsKs", "QhQd", "7c6c"}, "Qs Jh 2c");
  CHECK(flop.deals == 903);  // C(43, 2)
  CHECK_THAT(flop.players[0].equity, WithinAbs(0.1583610188261351, 1e-12));
  CHECK_THAT(flop.players[1].equity, WithinAbs(0.81063122923588038, 1e-12));
  CHECK_THAT(flop.players[2].equity, WithinAbs(0.031007751937984496, 1e-12));

  const EquityResult turn = exact({"AcKd", "JhTh"}, "9s 8c 2d 3h");
  CHECK(turn.deals == 44);
  CHECK_THAT(turn.players[0].equity, WithinAbs(30.0 / 44, 1e-15));  // 30 of the 44 rivers
  CHECK_THAT(turn.players[1].equity, WithinAbs(14.0 / 44, 1e-15));
}

TEST_CASE("exact equity: complete board is a single showdown", "[equity]") {
  const EquityResult river = exact({"AhAs", "KdKc"}, "Kh 7c 2d 9s 3h");
  CHECK(river.deals == 1);
  CHECK(river.players[0].equity == 0.0);  // kings make a set
  CHECK(river.players[1].equity == 1.0);

  // Both play the royal flush on the board: a split pot.
  const EquityResult split = exact({"2c3d", "4h5c"}, "As Ks Qs Js Ts");
  CHECK(split.players[0].tie == 1.0);
  CHECK(split.players[0].equity == 0.5);
  CHECK(split.players[1].equity == 0.5);
}

TEST_CASE("exact equity: consistency and symmetry", "[equity]") {
  const EquityResult r = exact({"Ac2d", "7h7s", "KsQs"});
  CHECK_THAT(total_equity(r), WithinAbs(1.0, 1e-12));
  for (const nuts::PlayerEquity& p : r.players) {
    CHECK(p.win >= 0);
    CHECK(p.win + p.tie <= 1.0);
    CHECK(p.equity >= p.win);
    CHECK(p.equity <= p.win + p.tie);
  }

  // Swapping the suits hearts <-> spades turns one hand into the other, so each has equity 1/2.
  // The win/tie counts are identical integers, so the doubles are exactly equal.
  const EquityResult same = exact({"AhKh", "AsKs"});
  CHECK(same.players[0].equity == same.players[1].equity);
  CHECK(same.players[0].win == same.players[1].win);
  CHECK_THAT(same.players[0].equity, WithinAbs(0.5, 1e-15));

  const EquityResult three = exact({"AhKh", "AsKs", "AdKd"});
  CHECK(three.players[0].equity == three.players[1].equity);
  CHECK(three.players[1].equity == three.players[2].equity);
  CHECK_THAT(three.players[0].equity, WithinAbs(1.0 / 3, 1e-12));

  // Listing the players in the other order swaps the results.
  const EquityResult forward = exact({"8c8d", "AhKs"}, "2c 7d 9h");
  const EquityResult backward = exact({"AhKs", "8c8d"}, "2c 7d 9h");
  CHECK(forward.players[0].equity == backward.players[1].equity);
  CHECK(forward.players[1].equity == backward.players[0].equity);
}

TEST_CASE("exact equity: invalid input", "[equity]") {
  std::string error;
  const auto check_invalid = [&](const std::vector<HoleCards>& hole_cards,
                                 const std::vector<nuts::Card>& board_cards) {
    error.clear();
    CHECK_FALSE(nuts::equity_exact(hole_cards, board_cards, &error).has_value());
    CHECK_FALSE(error.empty());
  };
  check_invalid(hands({"AhAs"}), {});                         // one player
  check_invalid(hands({"AhAs", "AhKd"}), {});                 // Ah twice
  check_invalid(hands({"AhAs", "KdKc"}), board("Ah 2c 3d"));  // Ah in a hand and on the board
  check_invalid(hands({"AhAs", "KdKc"}), board("2c 3d 4h 5s 6c 7d"));  // six board cards
  check_invalid({HoleCards{52, 0}, HoleCards{1, 2}}, {});              // card id 52
  check_invalid({HoleCards{5, 5}, HoleCards{1, 2}}, {});  // same card twice in one hand

  // 24 players do not fit (24 * 2 + 5 > 52).
  std::vector<HoleCards> many;
  for (int i = 0; i < 24; ++i) {
    many.push_back({static_cast<nuts::Card>(2 * i + 1), static_cast<nuts::Card>(2 * i)});
  }
  check_invalid(many, {});
  // 23 players do (6 cards left, C(6, 5) = 6 boards).
  many.pop_back();
  const std::optional<EquityResult> full_table = nuts::equity_exact(many, {});
  REQUIRE(full_table.has_value());
  CHECK(full_table->deals == 6);
  CHECK_THAT(total_equity(*full_table), WithinAbs(1.0, 1e-12));
}

// ---- Monte Carlo ----------------------------------------------------------------------------

TEST_CASE("Monte Carlo agrees with exact enumeration", "[equity]") {
  const EquityResult truth = exact({"AhAs", "KdKc"});
  for (const int threads : {1, 4}) {
    INFO("threads = " << threads);
    const EquityResult mc = monte_carlo({"AhAs", "KdKc"}, "", 200'000, 1, threads);
    CHECK_FALSE(mc.exact);
    CHECK(mc.deals == 200'000);
    CHECK(mc.threads == threads);
    check_within_4_se(mc.players[0], truth.players[0].equity);
    check_within_4_se(mc.players[1], truth.players[1].equity);
  }

  const EquityResult truth3 = exact({"8c8d", "AhKs", "Td9d"}, "2c 7d 9h");
  const EquityResult mc3 = monte_carlo({"8c8d", "AhKs", "Td9d"}, "2c 7d 9h", 200'000, 2, 3);
  for (std::size_t p = 0; p < 3; ++p) check_within_4_se(mc3.players[p], truth3.players[p].equity);

  // The standard error is sqrt(variance / trials). For AA vs KK the share X is 1 about 81% of
  // the time, so its standard deviation is about 0.39 and 200,000 trials give about 0.00087.
  const EquityResult mc = monte_carlo({"AhAs", "KdKc"}, "", 200'000, 1, 1);
  CHECK(mc.players[0].std_error > 0.0008);
  CHECK(mc.players[0].std_error < 0.0010);
}

TEST_CASE("Monte Carlo over ranges agrees with exact range equity", "[equity]") {
  // On a flop, so the exact reference (every combo pair, every turn and river) stays cheap.
  const std::vector<nuts::Card> flop = board("Ks 7h 2d");
  const std::vector<std::pair<const char*, const char*>> matchups = {
      {"AA, KK", "QQ+, AK"},       // card removal: AA/KK combos clash with each other and the K
      {"AA:0.5, 72o", "KQs, 77"},  // weights, and a set of sevens
      {"22-55", "AKs"},
  };
  std::uint64_t seed = 10;
  for (const auto& [first, second] : matchups) {
    INFO(first << " vs " << second);
    const std::vector<Range> player_ranges = ranges({first, second});
    const std::vector<double> truth = exact_range_equity(player_ranges, flop);
    const std::optional<EquityResult> mc =
        nuts::equity_monte_carlo(player_ranges, flop, 200'000, seed++, 2);
    REQUIRE(mc.has_value());
    check_within_4_se(mc->players[0], truth[0]);
    check_within_4_se(mc->players[1], truth[1]);
  }
}

TEST_CASE("Monte Carlo is deterministic for a given seed and thread count", "[equity]") {
  for (const int threads : {1, 3, 8}) {
    INFO("threads = " << threads);
    const EquityResult a = monte_carlo({"QQ+, AKs", "random"}, "", 50'001, 42, threads);
    const EquityResult b = monte_carlo({"QQ+, AKs", "random"}, "", 50'001, 42, threads);
    CHECK(a.deals == 50'001);
    for (std::size_t p = 0; p < 2; ++p) {
      // Bit-for-bit equal, not just close.
      CHECK(a.players[p].equity == b.players[p].equity);
      CHECK(a.players[p].win == b.players[p].win);
      CHECK(a.players[p].tie == b.players[p].tie);
      CHECK(a.players[p].std_error == b.players[p].std_error);
    }
  }
  // A different seed gives a different sample.
  const EquityResult a = monte_carlo({"QQ+, AKs", "random"}, "", 50'001, 42, 3);
  const EquityResult c = monte_carlo({"QQ+, AKs", "random"}, "", 50'001, 43, 3);
  CHECK(a.players[0].equity != c.players[0].equity);
}

TEST_CASE("Monte Carlo sanity cases", "[equity]") {
  // AA against a random hand wins about 85% of the time.
  const EquityResult aa = monte_carlo({"AA", "random"}, "", 100'000, 7, 2);
  CHECK(aa.players[0].equity > 0.8);
  CHECK(aa.players[0].equity < 0.9);
  CHECK_THAT(total_equity(aa), WithinAbs(1.0, 1e-12));

  // Identical ranges are symmetric: each player has equity 1/2 on average.
  const EquityResult same = monte_carlo({"random", "random"}, "", 100'000, 8, 2);
  check_within_4_se(same.players[0], 0.5);
  check_within_4_se(same.players[1], 0.5);
  const EquityResult three = monte_carlo({"TT+", "TT+", "TT+"}, "", 100'000, 9, 2);
  for (const nuts::PlayerEquity& p : three.players) check_within_4_se(p, 1.0 / 3);

  // Combos that use a board card are removed from the ranges: with As and Ah on the board, AdAc
  // is the only AA combo left, so "AA" and "AdAc" give the same result (same seed, same draws).
  const EquityResult blocked = monte_carlo({"AA", "KK"}, "As Ah 2c", 20'000, 11, 1);
  const EquityResult specific = monte_carlo({"AdAc", "KK"}, "As Ah 2c", 20'000, 11, 1);
  CHECK(blocked.players[0].equity == specific.players[0].equity);

  // More threads than trials: one trial per thread.
  const EquityResult tiny = monte_carlo({"AA", "KK"}, "", 3, 1, 8);
  CHECK(tiny.threads == 3);
  CHECK(tiny.deals == 3);

  // threads = 0 uses the hardware threads.
  const EquityResult automatic = monte_carlo({"AA", "KK"}, "", 1'000, 1, 0);
  CHECK(automatic.threads >= 1);
  CHECK(automatic.deals == 1'000);
}

TEST_CASE("Monte Carlo: players with any two cards are dealt from the leftover deck", "[equity]") {
  // A range of every combo ("random") is dealt from the cards the other players left, so even a
  // full table of 23 random hands deals at once. (Trial and error over whole deals would almost
  // never succeed: all 23 players avoid each other's cards with probability about 1e-17.)
  for (const std::size_t num_players : {16, 23}) {
    INFO(num_players << " players");
    const std::vector<Range> table(num_players, ranges({"random"})[0]);
    std::string error;
    const std::optional<EquityResult> result =
        nuts::equity_monte_carlo(table, board("2c 7d 9h"), 20'000, 5, 1, &error);
    INFO(error);
    REQUIRE(result.has_value());
    CHECK_THAT(total_equity(*result), WithinAbs(1.0, 1e-9));
    // Everyone has the same range, so everyone has the same equity: 1 / num_players.
    for (const nuts::PlayerEquity& p : result->players) {
      check_within_4_se(p, 1.0 / static_cast<double>(num_players));
    }
  }
  // Mixed with ranges that are dealt by trial and error.
  const EquityResult mixed = monte_carlo({"AA", "random", "KK", "random", "random", "random",
                                          "random", "random", "random", "random", "random",
                                          "random", "random", "random", "random", "random"},
                                         "", 5'000, 6, 2);
  CHECK_THAT(total_equity(mixed), WithinAbs(1.0, 1e-9));
  CHECK(mixed.deals == 5'000);
}

TEST_CASE("Monte Carlo: any two cards gives the same distribution as its range", "[equity]") {
  // Against the exact range equity (every combo of each range, every turn and river).
  const std::vector<nuts::Card> flop = board("Ks 7h 2d");
  const std::vector<Range> player_ranges = ranges({"AA, KK", "random"});
  const std::vector<double> truth = exact_range_equity(player_ranges, flop);
  const std::optional<EquityResult> mc =
      nuts::equity_monte_carlo(player_ranges, flop, 200'000, 20, 2);
  REQUIRE(mc.has_value());
  check_within_4_se(mc->players[0], truth[0]);
  check_within_4_se(mc->players[1], truth[1]);

  // Against trial and error: giving one combo of "random" a slightly different weight makes the
  // range no longer "any two cards", so it is dealt by trial and error like any other range.
  // The two ways must agree (the weight change itself moves the equity by far less than the
  // standard error).
  const EquityResult shortcut = monte_carlo({"TT+", "random", "random"}, "", 200'000, 21, 2);
  const EquityResult rejection = monte_carlo(
      {"TT+", "random, 3d2c:0.999999", "random, 3d2c:0.999999"}, "", 200'000, 22, 2);
  for (std::size_t p = 0; p < 3; ++p) {
    INFO("player " << p + 1);
    const double se = std::hypot(shortcut.players[p].std_error, rejection.players[p].std_error);
    CHECK(std::abs(shortcut.players[p].equity - rejection.players[p].equity) < 4 * se);
  }
}

TEST_CASE("Monte Carlo: huge weights do not overflow", "[equity]") {
  // The C++ API accepts any finite weight. The weights are divided by the largest one before
  // they are added up, so 2^1023 and 1.5 * 2^1023 (whose sum is too large for a double) mean the
  // same as 1 and 1.5, and give bit-for-bit the same result.
  const auto weighted = [](double aces, double kings, double queens) {
    return Range{nuts::Combo{*nuts::parse_hole_cards("AsAh"), aces},
                 nuts::Combo{*nuts::parse_hole_cards("KsKh"), kings},
                 nuts::Combo{*nuts::parse_hole_cards("QsQh"), queens}};
  };
  const Range random = ranges({"random"})[0];
  const std::vector<Range> small = {weighted(1, 1.5, 1), random};
  const std::vector<Range> huge = {
      weighted(std::ldexp(1.0, 1023), std::ldexp(1.5, 1023), std::ldexp(1.0, 1023)), random};
  const std::optional<EquityResult> a = nuts::equity_monte_carlo(small, {}, 50'000, 3, 1);
  const std::optional<EquityResult> b = nuts::equity_monte_carlo(huge, {}, 50'000, 3, 1);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->players[0].equity == b->players[0].equity);
}

TEST_CASE("Monte Carlo: thread count limit", "[equity]") {
  // Asking for more threads than kMaxThreads is an error, not a crash. (Before the limit, 5,000
  // threads made the operating system refuse to start one, and the program was terminated.)
  std::string error;
  const std::vector<Range> aa_kk = ranges({"AA", "KK"});
  CHECK_FALSE(
      nuts::equity_monte_carlo(aa_kk, {}, 10'000, 1, nuts::kMaxThreads + 1, &error).has_value());
  CHECK(error.find("threads") != std::string::npos);
  CHECK_FALSE(nuts::equity_monte_carlo(aa_kk, {}, 10'000, 1, 5'000, &error).has_value());

  // kMaxThreads itself works.
  const std::optional<EquityResult> most =
      nuts::equity_monte_carlo(aa_kk, {}, 1'024, 1, nuts::kMaxThreads, &error);
  INFO(error);
  REQUIRE(most.has_value());
  CHECK(most->threads == nuts::kMaxThreads);
  CHECK(most->deals == 1'024);
}

TEST_CASE("Monte Carlo: invalid input", "[equity]") {
  std::string error;
  const auto check_invalid = [&](const std::vector<Range>& player_ranges,
                                 const std::vector<nuts::Card>& board_cards,
                                 std::uint64_t trials = 1'000) {
    error.clear();
    CHECK_FALSE(
        nuts::equity_monte_carlo(player_ranges, board_cards, trials, 1, 2, &error).has_value());
    CHECK_FALSE(error.empty());
  };
  check_invalid(ranges({"AA"}), {});                       // one player
  check_invalid(ranges({"AA", "KK"}), {}, 0);              // no trials
  check_invalid(ranges({"AA", ""}), {});                   // empty range
  check_invalid(ranges({"AA", "KK"}), board("As Ah Ad"));  // no AA left without a board ace
  check_invalid(ranges({"AA", "KK"}), {0, 0});             // 2c twice on the board
  check_invalid(ranges({"AA", "AA", "AA"}), {});           // only 4 aces for 3 players
  check_invalid({Range{nuts::Combo{{52, 1}, 1.0}}, ranges({"KK"})[0]}, {});  // bad card id
  check_invalid({Range{nuts::Combo{{3, 3}, 1.0}}, ranges({"KK"})[0]}, {});   // repeated card
  check_invalid({Range{nuts::Combo{{3, 2}, -1.0}}, ranges({"KK"})[0]}, {});  // negative weight
}
