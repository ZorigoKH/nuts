#include "nuts/card.hpp"

#include <bit>

namespace nuts {
namespace {

bool is_separator(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

std::optional<int> parse_rank(char c) {
  const char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  const std::size_t pos = kRankChars.find(upper);
  if (pos == std::string_view::npos) return std::nullopt;
  return static_cast<int>(pos);
}

std::optional<int> parse_suit(char c) {
  const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  const std::size_t pos = kSuitChars.find(lower);
  if (pos == std::string_view::npos) return std::nullopt;
  return static_cast<int>(pos);
}

std::uint64_t mask_of(std::span<const Card> cards) noexcept {
  std::uint64_t mask = 0;
  for (const Card card : cards) mask |= card_bit(card);
  return mask;
}

std::vector<Card> cards_of(std::uint64_t mask) {
  mask &= kFullDeckMask;
  std::vector<Card> cards;
  cards.reserve(static_cast<std::size_t>(std::popcount(mask)));
  while (mask != 0) {
    cards.push_back(static_cast<Card>(std::countr_zero(mask)));  // index of the lowest set bit
    mask &= mask - 1;                                            // clear the lowest set bit
  }
  return cards;
}

std::optional<Card> parse_card(std::string_view text) {
  std::optional<int> rank;
  char suit_char = 0;
  if (text.size() == 2) {
    rank = parse_rank(text[0]);
    suit_char = text[1];
  } else if (text.size() == 3 && text.substr(0, 2) == "10") {
    rank = kTen;
    suit_char = text[2];
  } else {
    return std::nullopt;
  }
  const std::optional<int> suit = parse_suit(suit_char);
  if (!rank || !suit) return std::nullopt;
  return make_card(*rank, *suit);
}

std::optional<std::vector<Card>> parse_cards(std::string_view text) {
  std::vector<Card> cards;
  std::uint64_t seen = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    if (is_separator(text[i])) {
      ++i;
      continue;
    }
    // A card is two characters ("As"), or three when the rank is written as "10" ("10s").
    const std::size_t length = text.substr(i, 2) == "10" ? 3 : 2;
    const std::optional<Card> card = parse_card(text.substr(i, length));
    if (!card) return std::nullopt;
    if (seen & card_bit(*card)) return std::nullopt;  // duplicate card
    seen |= card_bit(*card);
    cards.push_back(*card);
    i += length;
  }
  return cards;
}

std::string to_string(Card card) {
  if (!is_valid_card(card)) return "??";
  return {kRankChars[static_cast<std::size_t>(rank_of(card))],
          kSuitChars[static_cast<std::size_t>(suit_of(card))]};
}

std::string to_string(std::span<const Card> cards) {
  std::string out;
  for (const Card card : cards) {
    if (!out.empty()) out += ' ';
    out += to_string(card);
  }
  return out;
}

}  // namespace nuts
