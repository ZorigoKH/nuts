// Kuhn poker: game rules, evaluation, best responses and vanilla CFR. See include/nuts/kuhn.hpp.
//
// The game tree is tiny (6 deals, at most 3 decisions per hand), so everything here simply walks
// the whole tree recursively, with the history of actions as a string ("", "p", "pb", ...).
#include "nuts/kuhn.hpp"

#include <algorithm>
#include <cstddef>

namespace nuts {
namespace {

constexpr int kKuhnCards = 3;  // J = 0, Q = 1, K = 2
constexpr std::string_view kCardNames = "JQK";
constexpr std::string_view kActions = "pb";  // action 0 = p (check/fold), 1 = b (bet/call)

// The histories at which someone has to decide, in information-set index order.
constexpr std::array<std::string_view, 4> kDecisionHistories = {"", "p", "b", "pb"};

// The six deals (player 1's card, player 2's card); each has probability 1/6.
constexpr std::array<std::array<int, 2>, 6> kDeals = {
    {{0, 1}, {0, 2}, {1, 0}, {1, 2}, {2, 0}, {2, 1}}};
constexpr double kDealProbability = 1.0 / 6;

// The information set of a player holding `card` who must act after `history`.
int info_set_index(int card, std::string_view history) {
  for (std::size_t h = 0; h < kDecisionHistories.size(); ++h) {
    if (kDecisionHistories[h] == history) return 4 * card + static_cast<int>(h);
  }
  return -1;  // not a decision point (never happens for the histories we build)
}

// The players alternate, player 1 (index 0) first.
int player_to_act(std::string_view history) { return static_cast<int>(history.size() % 2); }

// The game ends after these histories.
bool is_terminal(std::string_view history) {
  return history == "pp" || history == "bp" || history == "bb" || history == "pbp" ||
         history == "pbb";
}

// Player 1's result at the end of the game (`history` must be terminal). Player 2's result is
// the negative (the game is zero-sum).
double payoff(std::string_view history, int card1, int card2) {
  if (history == "bp") return 1;                 // player 2 folded to player 1's bet
  if (history == "pbp") return -1;               // player 1 folded to player 2's bet
  const double winner = card1 > card2 ? 1 : -1;  // showdown: the higher card wins
  return history == "pp" ? winner : 2 * winner;  // the antes, or the antes and a called bet
}

// Player 1's expected result from `history` on, when both players follow `strategy`.
double expected_value(const KuhnStrategy& strategy, const std::array<int, 2>& cards,
                      const std::string& history) {
  if (is_terminal(history)) return payoff(history, cards[0], cards[1]);
  const int player = player_to_act(history);
  const double bet = strategy[static_cast<std::size_t>(
      info_set_index(cards[static_cast<std::size_t>(player)], history))];
  return (1 - bet) * expected_value(strategy, cards, history + 'p') +
         bet * expected_value(strategy, cards, history + 'b');
}

// The best total result the `responder` can reach from `history` on while holding `card`, against
// the other player's part of `strategy`. The total is over the opponent's possible cards c, each
// weighted by opponent_weight[c]: the probability of the deal times the probability that the
// opponent, holding c, took its actions so far.
//
// The responder cannot see the opponent's card, so at each of its decisions it must pick one
// action for all of them: the one with the larger total. (Taking the maximum separately for each
// opponent card would be cheating.) The responder's later decisions are optimized first, by the
// recursion, which is fine because a player always remembers its own earlier actions.
double best_response(const KuhnStrategy& strategy, int responder, int card,
                     const std::string& history,
                     const std::array<double, kKuhnCards>& opponent_weight) {
  if (is_terminal(history)) {
    double total = 0;
    for (int c = 0; c < kKuhnCards; ++c) {
      const double weight = opponent_weight[static_cast<std::size_t>(c)];
      if (weight == 0) continue;
      const double player1_result = responder == 0 ? payoff(history, card, c)  // responder is 1
                                                   : payoff(history, c, card);
      total += weight * (responder == 0 ? player1_result : -player1_result);
    }
    return total;
  }
  if (player_to_act(history) == responder) {
    return std::max(best_response(strategy, responder, card, history + 'p', opponent_weight),
                    best_response(strategy, responder, card, history + 'b', opponent_weight));
  }
  // The opponent decides; how likely each action is depends on the opponent's card.
  std::array<double, kKuhnCards> after_pass{};
  std::array<double, kKuhnCards> after_bet{};
  for (int c = 0; c < kKuhnCards; ++c) {
    const auto k = static_cast<std::size_t>(c);
    const double bet = strategy[static_cast<std::size_t>(info_set_index(c, history))];
    after_pass[k] = opponent_weight[k] * (1 - bet);
    after_bet[k] = opponent_weight[k] * bet;
  }
  return best_response(strategy, responder, card, history + 'p', after_pass) +
         best_response(strategy, responder, card, history + 'b', after_bet);
}

// The value of a best response for `responder` (0 or 1), from that player's point of view.
double best_response_value(const KuhnStrategy& strategy, int responder) {
  double total = 0;
  for (int card = 0; card < kKuhnCards; ++card) {
    std::array<double, kKuhnCards> opponent_weight{};
    for (int c = 0; c < kKuhnCards; ++c) {
      if (c != card) opponent_weight[static_cast<std::size_t>(c)] = kDealProbability;
    }
    total += best_response(strategy, responder, card, "", opponent_weight);
  }
  return total;
}

// Vanilla CFR. Each iteration:
//   1. turns the cumulative regrets into the current strategy (regret matching);
//   2. walks the whole game tree for every deal with that strategy, adding each information
//      set's counterfactual regrets and its reach-weighted strategy (for the average).
// The strategy is fixed during an iteration, so all information sets are updated at once from
// the same strategy ("simultaneous updates").
class KuhnCfr {
 public:
  void iterate() {
    for (std::size_t i = 0; i < kKuhnNumInfoSets; ++i) {
      // Regret matching: play each action in proportion to its positive cumulative regret, or
      // both equally often if neither is positive.
      const double pass = std::max(regret_sum_[i][0], 0.0);
      const double bet = std::max(regret_sum_[i][1], 0.0);
      current_[i] = pass + bet > 0 ? bet / (pass + bet) : 0.5;
    }
    for (const std::array<int, 2>& cards : kDeals) traverse(cards, "", {1.0, 1.0});
  }

  // Each information set's strategy averaged over the iterations, each iteration weighted by
  // the probability that the player itself plays to that information set.
  KuhnStrategy average_strategy() const {
    KuhnStrategy average{};
    for (std::size_t i = 0; i < kKuhnNumInfoSets; ++i) {
      average[i] = reach_sum_[i] > 0 ? bet_sum_[i] / reach_sum_[i] : 0.5;
    }
    return average;
  }

 private:
  // Returns player 1's expected result from `history` on under the current strategy. reach[p] is
  // the probability that player p's own actions lead to `history`.
  double traverse(const std::array<int, 2>& cards, const std::string& history,
                  const std::array<double, 2>& reach) {
    if (is_terminal(history)) return payoff(history, cards[0], cards[1]);
    const int player = player_to_act(history);
    const auto p = static_cast<std::size_t>(player);
    const auto info = static_cast<std::size_t>(info_set_index(cards[p], history));
    const std::array<double, 2> probability = {1 - current_[info], current_[info]};

    std::array<double, 2> action_value{};
    double node_value = 0;
    for (std::size_t a = 0; a < 2; ++a) {
      std::array<double, 2> next_reach = reach;
      next_reach[p] *= probability[a];
      action_value[a] = traverse(cards, history + kActions[a], next_reach);
      node_value += probability[a] * action_value[a];
    }

    // Counterfactual regret of each action: how much better it would have done than the current
    // strategy, for the player acting here (player 2 wants player 1's result to be low), weighted
    // by the probability that chance and the opponent bring the game here.
    const double sign = player == 0 ? 1.0 : -1.0;
    const double others_reach = kDealProbability * reach[1 - p];
    for (std::size_t a = 0; a < 2; ++a) {
      regret_sum_[info][a] += others_reach * sign * (action_value[a] - node_value);
    }
    bet_sum_[info] += reach[p] * probability[1];
    reach_sum_[info] += reach[p];
    return node_value;
  }

  std::array<std::array<double, 2>, kKuhnNumInfoSets> regret_sum_{};  // [info set][action]
  std::array<double, kKuhnNumInfoSets> bet_sum_{};    // sum of reach * P(b), for the average
  std::array<double, kKuhnNumInfoSets> reach_sum_{};  // sum of reach
  KuhnStrategy current_{};                            // P(b) this iteration
};

}  // namespace

std::string kuhn_info_set_name(int index) {
  if (index < 0 || index >= kKuhnNumInfoSets) return "";
  return kCardNames[static_cast<std::size_t>(index / 4)] +
         std::string(kDecisionHistories[static_cast<std::size_t>(index % 4)]);
}

std::optional<int> kuhn_info_set_index(std::string_view name) {
  for (int index = 0; index < kKuhnNumInfoSets; ++index) {
    if (kuhn_info_set_name(index) == name) return index;
  }
  return std::nullopt;
}

double kuhn_game_value(const KuhnStrategy& strategy) {
  double total = 0;
  for (const std::array<int, 2>& cards : kDeals) {
    total += kDealProbability * expected_value(strategy, cards, "");
  }
  return total;
}

KuhnExploitability kuhn_exploitability(const KuhnStrategy& strategy) {
  KuhnExploitability result;
  result.player1_best_response = best_response_value(strategy, 0);
  result.player2_best_response = best_response_value(strategy, 1);
  result.exploitability = (result.player1_best_response + result.player2_best_response) / 2;
  return result;
}

KuhnResult kuhn_cfr(int iterations) {
  KuhnResult result;
  result.iterations = std::max(iterations, 1);
  KuhnCfr cfr;
  for (int t = 0; t < result.iterations; ++t) cfr.iterate();
  result.average_strategy = cfr.average_strategy();
  result.game_value = kuhn_game_value(result.average_strategy);
  result.exploitability = kuhn_exploitability(result.average_strategy).exploitability;
  return result;
}

}  // namespace nuts
