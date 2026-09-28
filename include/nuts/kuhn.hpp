// Kuhn poker (Kuhn, 1950) solved with vanilla counterfactual regret minimization (CFR, Zinkevich
// et al., 2007). It is the smallest interesting poker game, and its exact solution is known, so
// it is the standard correctness test for a CFR implementation.
//
// The game. The deck has three cards, J < Q < K. Each player antes 1 and gets one card (the third
// card is not used). Then:
//
//   player 1: check ("p", for pass) or bet 1 ("b")
//     after p, player 2: check (p)  -> showdown for the antes: the higher card wins 1
//                        bet (b)    -> player 1: fold (p) -> player 2 wins 1
//                                                call (b) -> showdown: the higher card wins 2
//     after b, player 2: fold (p)   -> player 1 wins 1
//                        call (b)   -> showdown: the higher card wins 2
//
// So every decision is between two actions, "p" (check or fold) and "b" (bet or call), and a
// history of actions is a string such as "pb". A player decides knowing only its own card and
// the history: that pair is an information set, written like "Kpb" (holding K, after check-bet).
// There are 12: player 1 decides at "" and "pb", player 2 at "p" and "b", each with 3 cards.
//
// Every Nash equilibrium gives player 1 the value -1/18 per hand (player 2's position is better).
// Player 1 has a family of equilibrium strategies with a parameter alpha in [0, 1/3]: bet J with
// probability alpha, bet K with 3 * alpha, call "pb" with Q with alpha + 1/3. Player 2's
// equilibrium strategy is unique.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace nuts {

inline constexpr int kKuhnNumInfoSets = 12;

// A strategy for both players: for each information set, the probability of "b" (bet or call);
// "p" has the rest. Index: see kuhn_info_set_name().
using KuhnStrategy = std::array<double, kKuhnNumInfoSets>;

// The name of information set 0..11: "J", "Jp", "Jb", "Jpb", "Q", "Qp", ..., "Kpb". The index is
// 4 * card + position of the history in {"", "p", "b", "pb"}, with card J = 0, Q = 1, K = 2.
std::string kuhn_info_set_name(int index);

// The index of a name such as "Kpb", or std::nullopt.
std::optional<int> kuhn_info_set_index(std::string_view name);

// Player 1's expected result per hand (in antes) when both players follow `strategy`.
double kuhn_game_value(const KuhnStrategy& strategy);

// How much each player could win by switching to a best response (the best strategy against the
// other player's part of `strategy`), computed exactly.
struct KuhnExploitability {
  double player1_best_response = 0;  // player 1's value with a best response to player 2
  double player2_best_response = 0;  // player 2's value with a best response to player 1
  // The average of what the two best responses gain:
  //   ((player1_best_response - v) + (player2_best_response - (-v))) / 2 with v the game value,
  // which simplifies to (player1_best_response + player2_best_response) / 2. It is 0 exactly
  // when `strategy` is a Nash equilibrium.
  double exploitability = 0;
};

KuhnExploitability kuhn_exploitability(const KuhnStrategy& strategy);

struct KuhnResult {
  int iterations = 0;
  KuhnStrategy average_strategy{};  // the CFR average strategy, which approaches equilibrium
  double game_value = 0;            // kuhn_game_value(average_strategy)
  double exploitability = 0;        // kuhn_exploitability(average_strategy).exploitability
};

// Runs `iterations` iterations of vanilla CFR (at least 1; fewer are treated as 1).
KuhnResult kuhn_cfr(int iterations);

}  // namespace nuts
