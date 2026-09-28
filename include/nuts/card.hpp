// Cards, parsing/formatting, and 64-bit card masks.
//
// A card is a small integer 0..51:
//
//     id = 4 * rank + suit
//
//     rank: 0..12 = 2, 3, 4, 5, 6, 7, 8, 9, T, J, Q, K, A
//     suit: 0..3  = c (clubs), d (diamonds), h (hearts), s (spades)
//
// So 2c = 0, 2d = 1, ..., As = 51. With this layout, rank = id / 4 and suit = id % 4, and a set of
// cards fits in one 64-bit mask (bit `id` set = card present). In a mask, the 4 bits of each rank
// sit next to each other: bits 4r..4r+3 are the four suits of rank r. The evaluator relies on this.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nuts {

using Card = std::uint8_t;  // 0..51, see the encoding above

inline constexpr int kNumRanks = 13;
inline constexpr int kNumSuits = 4;
inline constexpr int kNumCards = 52;

// Rank indices, for readability in code and tests.
inline constexpr int kTwo = 0;
inline constexpr int kThree = 1;
inline constexpr int kFour = 2;
inline constexpr int kFive = 3;
inline constexpr int kSix = 4;
inline constexpr int kSeven = 5;
inline constexpr int kEight = 6;
inline constexpr int kNine = 7;
inline constexpr int kTen = 8;
inline constexpr int kJack = 9;
inline constexpr int kQueen = 10;
inline constexpr int kKing = 11;
inline constexpr int kAce = 12;

// Suit indices.
inline constexpr int kClubs = 0;
inline constexpr int kDiamonds = 1;
inline constexpr int kHearts = 2;
inline constexpr int kSpades = 3;

inline constexpr std::string_view kRankChars = "23456789TJQKA";
inline constexpr std::string_view kSuitChars = "cdhs";

// Precondition for all three: 0 <= rank < 13, 0 <= suit < 4, card < 52.
constexpr Card make_card(int rank, int suit) noexcept { return static_cast<Card>(4 * rank + suit); }
constexpr int rank_of(Card card) noexcept { return card / 4; }
constexpr int suit_of(Card card) noexcept { return card % 4; }

constexpr bool is_valid_card(Card card) noexcept { return card < kNumCards; }

// The mask bit of one card. An invalid id (>= 52) gives 0 instead of an out-of-range shift
// (shifting a 64-bit value by 64 or more is undefined behaviour), so callers can detect it by
// checking the popcount of the combined mask.
constexpr std::uint64_t card_bit(Card card) noexcept {
  return is_valid_card(card) ? std::uint64_t{1} << card : 0;
}

// Mask with all 52 cards set.
inline constexpr std::uint64_t kFullDeckMask = (std::uint64_t{1} << kNumCards) - 1;

// OR of card_bit() over the cards (duplicates collapse into one bit).
std::uint64_t mask_of(std::span<const Card> cards) noexcept;

// The cards in a mask, in increasing id order. Bits 52..63 are ignored.
std::vector<Card> cards_of(std::uint64_t mask);

// Parses a rank character: '2'..'9', 'T', 'J', 'Q', 'K', 'A' (either case) -> 0..12.
std::optional<int> parse_rank(char c);

// Parses a suit character: 'c', 'd', 'h', 's' (either case) -> 0..3.
std::optional<int> parse_suit(char c);

// Parses one card such as "As", "td", "Th", "10h". Rank and suit letters are case-insensitive;
// "10" is accepted as an alternative to "T". Returns std::nullopt for anything else.
std::optional<Card> parse_card(std::string_view text);

// Parses a list of cards such as "As Kd 7c", "As,Kd,7c" or "AsKd7c" (whitespace and commas are
// separators; cards may also be written back to back). An empty string gives an empty list.
// Returns std::nullopt if any card is malformed or a card appears twice.
std::optional<std::vector<Card>> parse_cards(std::string_view text);

// "As", "Td", "2c", ... (rank letter upper case, suit letter lower case). Returns "??" for an
// invalid id.
std::string to_string(Card card);

// Cards separated by single spaces, e.g. "As Kd 7c".
std::string to_string(std::span<const Card> cards);

}  // namespace nuts
