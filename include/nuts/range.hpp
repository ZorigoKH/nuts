// Hand ranges: text such as "TT+, AKs, KQo:0.5" turned into a list of weighted two-card combos.
//
// A player's hole cards are two different cards; there are C(52, 2) = 1,326 such "combos".
// Poker players describe sets of them with a shorthand, which parse_range() understands:
//
//   AA            a pair: all 6 combos of two aces
//   AKs, AKo      two ranks, suited (4 combos) or offsuit (12 combos)
//   AK            two ranks, suited or offsuit (16 combos)
//   TT+           a pair and every higher pair: TT, JJ, QQ, KK, AA
//   A2s+, K9o+    the top card fixed and the second card from the one given up to one below the
//                 top card: A2s+ = A2s, A3s, ..., AKs. (So T9s+ is just T9s.)
//   22-55         every pair from one to the other: 22, 33, 44, 55 (either order: 55-22)
//   K7o-KTo       the top card fixed and the second card from one to the other: K7o, K8o, K9o,
//                 KTo (either order). Both ends need the same top card and the same s/o suffix.
//   AsKs          one specific combo, written as two cards ("10s9s" also works)
//   random        all 1,326 combos
//
// Items are separated by commas and/or whitespace. Letters may be upper or lower case, ranks are
// 2-9, T, J, Q, K, A (ten is T in the shorthand) and the two ranks may come in either order
// (KA = AK).
//
// Any item may end in ":weight", a number from 0 to 1 saying how often its combos are in the
// range: in "AA, KK:0.5" the player has KK half as often as AA. The weight is written with a
// decimal point, whatever the program's locale ("0.5", ".25", "1"). The default weight is 1.
// When a combo is named more than once, the last weight given wins, and weight 0 removes it.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nuts/card.hpp"

namespace nuts {

inline constexpr int kNumHoleCardCombos = 1326;  // C(52, 2)

// Two different hole cards. make_hole_cards() puts the higher card id first, so each combo has
// exactly one representation.
using HoleCards = std::array<Card, 2>;

// Precondition: a and b are valid, different cards.
constexpr HoleCards make_hole_cards(Card a, Card b) noexcept {
  return a > b ? HoleCards{a, b} : HoleCards{b, a};
}

// Parses two cards such as "AsKs", "As Ks" or "10s9s" (higher card first in the result).
// Returns std::nullopt unless the text is exactly two valid, different cards.
std::optional<HoleCards> parse_hole_cards(std::string_view text);

// "AsKs": the two cards in the order stored, without a space. (A different name from
// to_string(), which would print "As Ks" for the same two cards.)
std::string hole_cards_to_string(const HoleCards& cards);

struct Combo {
  HoleCards cards{};
  double weight = 1.0;  // how often these hole cards are in the range, relative to weight 1
};

// A range: combos in the order they were first named, each at most once, all with weight > 0.
using Range = std::vector<Combo>;

// Parses a range written as described at the top of this file. Returns std::nullopt if the text
// has an item that cannot be parsed, and then also stores a message in *error (if error is not
// null). A text without items ("" or " , ") gives an empty range.
std::optional<Range> parse_range(std::string_view text, std::string* error = nullptr);

// A range holding one combo with weight 1.
inline Range single_combo_range(const HoleCards& cards) { return Range{Combo{cards, 1.0}}; }

}  // namespace nuts
