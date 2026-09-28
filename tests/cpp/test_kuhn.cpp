#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>

#include "nuts/kuhn.hpp"

namespace {

using Catch::Matchers::WithinAbs;
using nuts::KuhnStrategy;

constexpr double kGameValue = -1.0 / 18;  // player 1's value at every Nash equilibrium

double& at(KuhnStrategy& strategy, const char* name) {
  const std::optional<int> index = nuts::kuhn_info_set_index(name);
  REQUIRE(index.has_value());
  return strategy[static_cast<std::size_t>(*index)];
}

double at(const KuhnStrategy& strategy, const char* name) {
  const std::optional<int> index = nuts::kuhn_info_set_index(name);
  REQUIRE(index.has_value());
  return strategy[static_cast<std::size_t>(*index)];
}

// The known equilibrium with parameter alpha in [0, 1/3] (probabilities of "b": bet or call).
KuhnStrategy equilibrium(double alpha) {
  KuhnStrategy s{};
  // Player 1: bet J with alpha (a bluff), never bet Q, bet K with 3 alpha; after check-bet, call
  // with Q with alpha + 1/3, always with K, never with J.
  at(s, "J") = alpha;
  at(s, "Q") = 0;
  at(s, "K") = 3 * alpha;
  at(s, "Jpb") = 0;
  at(s, "Qpb") = alpha + 1.0 / 3;
  at(s, "Kpb") = 1;
  // Player 2 (unique): facing a bet, call with Q 1/3 of the time and always with K; after a
  // check, bet J 1/3 of the time (a bluff), never Q, always K.
  at(s, "Jb") = 0;
  at(s, "Qb") = 1.0 / 3;
  at(s, "Kb") = 1;
  at(s, "Jp") = 1.0 / 3;
  at(s, "Qp") = 0;
  at(s, "Kp") = 1;
  return s;
}

}  // namespace

TEST_CASE("Kuhn: information set names", "[kuhn]") {
  CHECK(nuts::kuhn_info_set_name(0) == "J");
  CHECK(nuts::kuhn_info_set_name(1) == "Jp");
  CHECK(nuts::kuhn_info_set_name(7) == "Qpb");
  CHECK(nuts::kuhn_info_set_name(11) == "Kpb");
  CHECK(nuts::kuhn_info_set_name(12).empty());
  for (int i = 0; i < nuts::kKuhnNumInfoSets; ++i) {
    CHECK(nuts::kuhn_info_set_index(nuts::kuhn_info_set_name(i)) == i);
  }
  CHECK_FALSE(nuts::kuhn_info_set_index("Kbb").has_value());  // the game is over after "bb"
  CHECK_FALSE(nuts::kuhn_info_set_index("A").has_value());
}

TEST_CASE("Kuhn: the known equilibria are unexploitable and worth -1/18", "[kuhn]") {
  for (const double alpha : {0.0, 1.0 / 6, 1.0 / 3}) {
    INFO("alpha = " << alpha);
    const KuhnStrategy s = equilibrium(alpha);
    CHECK_THAT(nuts::kuhn_game_value(s), WithinAbs(kGameValue, 1e-15));
    const nuts::KuhnExploitability e = nuts::kuhn_exploitability(s);
    CHECK_THAT(e.player1_best_response, WithinAbs(kGameValue, 1e-15));
    CHECK_THAT(e.player2_best_response, WithinAbs(-kGameValue, 1e-15));
    CHECK_THAT(e.exploitability, WithinAbs(0.0, 1e-15));
  }
}

TEST_CASE("Kuhn: best responses to a strategy worked out by hand", "[kuhn]") {
  // Both players always bet or call: every hand is a showdown for 2, so the value is 0.
  KuhnStrategy always_bet{};
  always_bet.fill(1.0);
  CHECK_THAT(nuts::kuhn_game_value(always_bet), WithinAbs(0.0, 1e-15));

  // Player 1's best response to a player 2 who always bets and calls: with J check and fold to
  // the bet (-1; betting would lose 2), with Q anything (0: +2 against J, -2 against K, or check
  // and fold for -1), with K bet (+2). Average (-1 + 0 + 2) / 3 = 1/3.
  // Player 2's best response to a player 1 who always bets: with J fold (-1), with Q call (0),
  // with K call (+2): also 1/3.
  const nuts::KuhnExploitability e = nuts::kuhn_exploitability(always_bet);
  CHECK_THAT(e.player1_best_response, WithinAbs(1.0 / 3, 1e-15));
  CHECK_THAT(e.player2_best_response, WithinAbs(1.0 / 3, 1e-15));
  CHECK_THAT(e.exploitability, WithinAbs(1.0 / 3, 1e-15));
}

TEST_CASE("Kuhn: vanilla CFR converges to an equilibrium", "[kuhn]") {
  const nuts::KuhnResult result = nuts::kuhn_cfr(100'000);
  CHECK(result.iterations == 100'000);
  INFO("value " << result.game_value << ", exploitability " << result.exploitability);
  CHECK(std::abs(result.game_value - kGameValue) < 1e-3);
  CHECK(result.exploitability < 1e-3);
  CHECK(result.exploitability >= 0);
  CHECK(result.exploitability == nuts::kuhn_exploitability(result.average_strategy).exploitability);

  // Player 2's equilibrium strategy is unique, so CFR must find it.
  const KuhnStrategy& s = result.average_strategy;
  const KuhnStrategy target = equilibrium(0);
  for (const char* name : {"Jb", "Qb", "Kb", "Jp", "Qp", "Kp"}) {
    INFO(name);
    CHECK_THAT(at(s, name), WithinAbs(at(target, name), 0.02));
  }
  // Player 1's must be one of the family: some alpha in [0, 1/3] with the relations above.
  const double alpha = at(s, "J");
  CHECK(alpha >= 0);
  CHECK(alpha <= 1.0 / 3 + 0.02);
  CHECK_THAT(at(s, "K"), WithinAbs(3 * alpha, 0.03));
  CHECK_THAT(at(s, "Qpb"), WithinAbs(alpha + 1.0 / 3, 0.02));
  CHECK_THAT(at(s, "Q"), WithinAbs(0.0, 0.02));
  CHECK_THAT(at(s, "Jpb"), WithinAbs(0.0, 0.02));
  CHECK_THAT(at(s, "Kpb"), WithinAbs(1.0, 0.02));
}

TEST_CASE("Kuhn: more iterations, less exploitable", "[kuhn]") {
  const double after_1k = nuts::kuhn_cfr(1'000).exploitability;
  const double after_10k = nuts::kuhn_cfr(10'000).exploitability;
  CHECK(after_10k < after_1k);
  CHECK(nuts::kuhn_cfr(0).iterations == 1);  // at least one iteration
}
