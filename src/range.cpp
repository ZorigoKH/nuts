// Range parsing. See include/nuts/range.hpp for the syntax.
//
// Every item is expanded into combos one "hand class" at a time. A hand class is a pair ("TT")
// or two different ranks with an optional suffix ("AK", "AKs", "AKo"); ranges such as "TT+",
// "A2s+" and "K7o-KTo" are just lists of hand classes.
#include "nuts/range.hpp"

#include <algorithm>
#include <cstdint>

namespace nuts {
namespace {

enum class Suits { Any, Suited, Offsuit };  // no suffix, "s" or "o"

struct HandClass {
  int high = 0;  // the higher rank (0..12 = 2..A)
  int low = 0;   // the lower rank; equal to `high` for a pair
  Suits suits = Suits::Any;

  bool is_pair() const { return high == low; }
};

char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool is_separator(char c) { return c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// "AA", "AK", "ka", "AKs", "AKo": two ranks (either order) and an optional s/o suffix. A pair
// cannot have a suffix ("AAs" is not a hand).
std::optional<HandClass> parse_hand_class(std::string_view text) {
  if (text.size() != 2 && text.size() != 3) return std::nullopt;
  const std::optional<int> first = parse_rank(text[0]);
  const std::optional<int> second = parse_rank(text[1]);
  if (!first || !second) return std::nullopt;

  HandClass hand_class{std::max(*first, *second), std::min(*first, *second), Suits::Any};
  if (text.size() == 3) {
    const char suffix = to_lower(text[2]);
    if (suffix == 's') {
      hand_class.suits = Suits::Suited;
    } else if (suffix == 'o') {
      hand_class.suits = Suits::Offsuit;
    } else {
      return std::nullopt;
    }
    if (hand_class.is_pair()) return std::nullopt;
  }
  return hand_class;
}

// Calls add(HoleCards) for every combo of the hand class: 6 for a pair, 4 suited, 12 offsuit,
// 16 for both. Suits go from spades down to clubs, so e.g. AKs gives AsKs, AhKh, AdKd, AcKc.
template <typename Add>
void add_hand_class(const HandClass& hand_class, Add& add) {
  for (int suit1 = kNumSuits - 1; suit1 >= 0; --suit1) {
    for (int suit2 = kNumSuits - 1; suit2 >= 0; --suit2) {
      if (hand_class.is_pair()) {
        // Two cards of the same rank need two different suits; count each pair of suits once.
        if (suit2 >= suit1) continue;
      } else {
        const bool suited = suit1 == suit2;
        if (hand_class.suits == Suits::Suited && !suited) continue;
        if (hand_class.suits == Suits::Offsuit && suited) continue;
      }
      add(make_hole_cards(make_card(hand_class.high, suit1), make_card(hand_class.low, suit2)));
    }
  }
}

// Expands one item without its weight ("random", "AsKs", "TT+", "A2s+", "22-55", "K7o-KTo",
// "AKs") by calling add(HoleCards) for each of its combos. Returns an error message, or an empty
// string on success.
template <typename Add>
std::string expand_item(std::string_view item, Add& add) {
  const auto is_random = [](std::string_view text) {
    constexpr std::string_view kRandom = "random";
    return text.size() == kRandom.size() &&
           std::equal(text.begin(), text.end(), kRandom.begin(),
                      [](char a, char b) { return to_lower(a) == b; });
  };
  if (is_random(item)) {
    for (int a = kNumCards - 1; a >= 0; --a) {
      for (int b = a - 1; b >= 0; --b)
        add(make_hole_cards(static_cast<Card>(a), static_cast<Card>(b)));
    }
    return {};
  }

  // One specific combo, e.g. "AsKs". (A hand class has at most 3 characters and two cards take
  // at least 4, so no text can be both.)
  if (const std::optional<HoleCards> cards = parse_hole_cards(item)) {
    add(*cards);
    return {};
  }

  // "TT+", "A2s+": from the given hand class upwards.
  if (item.size() >= 2 && item.back() == '+') {
    const std::optional<HandClass> start = parse_hand_class(item.substr(0, item.size() - 1));
    if (!start) return "'" + std::string(item) + "' is not a valid hand range";
    if (start->is_pair()) {
      for (int rank = start->high; rank <= kAce; ++rank) {
        add_hand_class(HandClass{rank, rank, Suits::Any}, add);
      }
    } else {
      for (int low = start->low; low < start->high; ++low) {
        add_hand_class(HandClass{start->high, low, start->suits}, add);
      }
    }
    return {};
  }

  // "22-55", "K7o-KTo": everything between two hand classes.
  if (const std::size_t dash = item.find('-'); dash != std::string_view::npos) {
    const std::optional<HandClass> from = parse_hand_class(item.substr(0, dash));
    const std::optional<HandClass> to = parse_hand_class(item.substr(dash + 1));
    if (!from || !to) return "'" + std::string(item) + "' is not a valid hand range";
    if (from->is_pair() && to->is_pair()) {
      for (int rank = std::min(from->high, to->high); rank <= std::max(from->high, to->high);
           ++rank) {
        add_hand_class(HandClass{rank, rank, Suits::Any}, add);
      }
      return {};
    }
    if (from->is_pair() || to->is_pair() || from->high != to->high || from->suits != to->suits) {
      return "'" + std::string(item) +
             "': both ends of a range must be pairs, or have the same top card and suffix";
    }
    for (int low = std::min(from->low, to->low); low <= std::max(from->low, to->low); ++low) {
      add_hand_class(HandClass{from->high, low, from->suits}, add);
    }
    return {};
  }

  // A single hand class, e.g. "AKs".
  if (const std::optional<HandClass> hand_class = parse_hand_class(item)) {
    add_hand_class(*hand_class, add);
    return {};
  }
  return "'" + std::string(item) + "' is not a hand, a hand range or two different cards";
}

// Parses the text after ':' in "AA:0.5": a number from 0 to 1, written as digits with at most
// one decimal point ("1", "0.5", ".25", "1.0"), at most 18 digits in all.
//
// Parsed by hand rather than with std::strtod, because strtod follows the program's locale: in a
// program that switched to a German locale, for example, it expects "0,5" and stops at the '.'
// of "0.5". (std::istringstream set to the "C" locale would also work, but it adds about 100 KB
// to the WebAssembly build.)
std::optional<double> parse_weight(std::string_view text) {
  std::uint64_t digits = 0;  // all the digits as one integer: 25 for "0.25"
  int num_digits = 0;
  int decimals = 0;  // how many digits come after the point: 2 for "0.25", which is 25 / 10^2
  bool seen_point = false;
  for (const char c : text) {
    if (c == '.' && !seen_point) {
      seen_point = true;
    } else if (c >= '0' && c <= '9') {
      if (++num_digits > 18) return std::nullopt;  // 19 digits might not fit in 64 bits
      digits = 10 * digits + static_cast<std::uint64_t>(c - '0');
      if (seen_point) ++decimals;
    } else {
      return std::nullopt;
    }
  }
  if (num_digits == 0) return std::nullopt;  // "" or "."
  // Powers of 10 up to 10^22 are exact doubles, and a division is rounded correctly, so with up
  // to 15 digits (exact as a double too) this is the double nearest to the decimal number.
  double power_of_10 = 1;
  for (int i = 0; i < decimals; ++i) power_of_10 *= 10;
  const double weight = static_cast<double>(digits) / power_of_10;
  if (weight > 1) return std::nullopt;
  return weight;
}

// Index of a combo in 0..52*52-1 (only pairs with cards[0] > cards[1] are used).
std::size_t combo_index(const HoleCards& cards) {
  return std::size_t{cards[0]} * kNumCards + cards[1];
}

}  // namespace

std::optional<HoleCards> parse_hole_cards(std::string_view text) {
  const std::optional<std::vector<Card>> cards = parse_cards(text);
  if (!cards || cards->size() != 2) return std::nullopt;  // parse_cards rejects repeated cards
  return make_hole_cards((*cards)[0], (*cards)[1]);
}

std::string hole_cards_to_string(const HoleCards& cards) {
  return to_string(cards[0]) + to_string(cards[1]);
}

std::optional<Range> parse_range(std::string_view text, std::string* error) {
  // weights[combo_index(c)] = the latest weight given for combo c, or -1 if it was not named yet.
  std::vector<double> weights(std::size_t{kNumCards} * kNumCards, -1.0);
  std::vector<HoleCards> first_named;  // combos in the order they were first named

  std::size_t pos = 0;
  while (pos < text.size()) {
    if (is_separator(text[pos])) {
      ++pos;
      continue;
    }
    std::size_t end = pos;
    while (end < text.size() && !is_separator(text[end])) ++end;
    const std::string_view item = text.substr(pos, end - pos);
    pos = end;

    // Split off an optional ":weight".
    std::string_view hands = item;
    double weight = 1.0;
    if (const std::size_t colon = item.find(':'); colon != std::string_view::npos) {
      hands = item.substr(0, colon);
      const std::optional<double> parsed = parse_weight(item.substr(colon + 1));
      if (!parsed) {
        if (error) *error = "'" + std::string(item) + "': the weight must be a number from 0 to 1";
        return std::nullopt;
      }
      weight = *parsed;
    }

    auto add = [&](const HoleCards& cards) {
      double& slot = weights[combo_index(cards)];
      if (slot < 0) first_named.push_back(cards);
      slot = weight;
    };
    const std::string message = expand_item(hands, add);
    if (!message.empty()) {
      if (error) *error = message;
      return std::nullopt;
    }
  }

  Range range;
  for (const HoleCards& cards : first_named) {
    const double weight = weights[combo_index(cards)];
    if (weight > 0) range.push_back(Combo{cards, weight});
  }
  return range;
}

}  // namespace nuts
