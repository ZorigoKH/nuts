#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <clocale>
#include <set>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/range.hpp"

namespace {

using nuts::Range;

// Parses a range that must be valid.
Range range(std::string_view text) {
  std::string error;
  const std::optional<Range> parsed = nuts::parse_range(text, &error);
  INFO("range '" << text << "': " << error);
  REQUIRE(parsed.has_value());
  return *parsed;
}

std::size_t combos(std::string_view text) { return range(text).size(); }

// The combos of a range as strings, e.g. {"AsKs", "AhKh", ...}, sorted.
std::vector<std::string> combo_strings(const Range& r) {
  std::vector<std::string> out;
  for (const nuts::Combo& combo : r) out.push_back(nuts::hole_cards_to_string(combo.cards));
  std::sort(out.begin(), out.end());
  return out;
}

bool is_suited(const nuts::Combo& c) {
  return nuts::suit_of(c.cards[0]) == nuts::suit_of(c.cards[1]);
}

}  // namespace

TEST_CASE("combo counts", "[range]") {
  // The counts from the spec.
  CHECK(combos("22+") == 78);
  CHECK(combos("AK") == 16);
  CHECK(combos("AKs") == 4);
  CHECK(combos("AKo") == 12);
  CHECK(combos("random") == 1326);
  CHECK(combos("A2s+") == 48);

  CHECK(combos("AA") == 6);
  CHECK(combos("TT+") == 30);
  CHECK(combos("22-55") == 24);
  CHECK(combos("55-22") == 24);
  CHECK(combos("K7o-KTo") == 48);
  CHECK(combos("KTo-K7o") == 48);
  CHECK(combos("K7-KT") == 64);
  CHECK(combos("K9s+") == 16);  // K9s, KTs, KJs, KQs
  CHECK(combos("T9s+") == 4);   // the kicker can only go up to one below the top card
  CHECK(combos("QT+") == 32);   // QT, QJ
  CHECK(combos("AsKs") == 1);
  CHECK(combos("AA,KK") == 12);
  CHECK(combos("AA KK, QQ") == 18);
  CHECK(combos("TT+, AQs+, AKo") == 30 + 8 + 12);
}

TEST_CASE("dash ranges do not depend on the order of their ends", "[range]") {
  CHECK(combo_strings(range("K7o-KTo")) == combo_strings(range("KTo-K7o")));
  CHECK(combo_strings(range("22-55")) == combo_strings(range("55-22")));
  CHECK(combo_strings(range("K7o-KTo")) == combo_strings(range("K7o,K8o,K9o,KTo")));
}

TEST_CASE("suffixes, case and rank order", "[range]") {
  const Range suited = range("AKs");
  CHECK(std::all_of(suited.begin(), suited.end(), is_suited));
  const Range offsuit = range("AKo");
  CHECK(std::none_of(offsuit.begin(), offsuit.end(), is_suited));

  const Range a2s = range("A2s+");
  for (const nuts::Combo& combo : a2s) {
    CHECK(nuts::rank_of(combo.cards[0]) == nuts::kAce);
    CHECK(nuts::rank_of(combo.cards[1]) < nuts::kAce);
    CHECK(is_suited(combo));
  }

  CHECK(combo_strings(range("ka")) == combo_strings(range("AK")));
  CHECK(combo_strings(range("aks")) == combo_strings(range("AKs")));
  CHECK(combo_strings(range("AKS")) == combo_strings(range("AKs")));
  CHECK(combo_strings(range("KAo")) == combo_strings(range("AKo")));
  CHECK(combos("RANDOM") == 1326);
}

TEST_CASE("specific combos", "[range]") {
  CHECK(combo_strings(range("AsKs")) == std::vector<std::string>{"AsKs"});
  CHECK(combo_strings(range("KsAs")) == std::vector<std::string>{"AsKs"});  // higher card first
  CHECK(combo_strings(range("10s9s")) == std::vector<std::string>{"Ts9s"});
  CHECK(combo_strings(range("AsKs, AsKs")) == std::vector<std::string>{"AsKs"});

  const std::optional<nuts::HoleCards> hole = nuts::parse_hole_cards("Kd As");
  REQUIRE(hole.has_value());
  CHECK(nuts::hole_cards_to_string(*hole) == "AsKd");
  CHECK_FALSE(nuts::parse_hole_cards("AsAs").has_value());
  CHECK_FALSE(nuts::parse_hole_cards("As").has_value());
  CHECK_FALSE(nuts::parse_hole_cards("AsKsQs").has_value());
}

TEST_CASE("random is every combo exactly once", "[range]") {
  const Range all = range("random");
  std::set<std::pair<int, int>> seen;
  for (const nuts::Combo& combo : all) {
    CHECK(combo.cards[0] > combo.cards[1]);
    CHECK(nuts::is_valid_card(combo.cards[0]));
    seen.insert({combo.cards[0], combo.cards[1]});
  }
  CHECK(seen.size() == 1326);
}

TEST_CASE("repeated combos are merged, first-named order is kept", "[range]") {
  CHECK(combos("AK, AKs") == 16);
  CHECK(combos("22+, 55") == 78);

  const Range r = range("KK, AA");
  REQUIRE(r.size() == 12);
  CHECK(nuts::rank_of(r.front().cards[0]) == nuts::kKing);
  CHECK(nuts::rank_of(r.back().cards[0]) == nuts::kAce);
}

TEST_CASE("weights", "[range]") {
  const Range r = range("AA:0.5, KK");
  REQUIRE(r.size() == 12);
  for (const nuts::Combo& combo : r) {
    CHECK(combo.weight == (nuts::rank_of(combo.cards[0]) == nuts::kAce ? 0.5 : 1.0));
  }

  // The last weight given for a combo wins; weight 0 removes it.
  const Range later = range("AA, AA:0.25");
  REQUIRE(later.size() == 6);
  CHECK(later.front().weight == 0.25);
  CHECK(combos("AK, AKs:0") == 12);
  CHECK(combos("AA:0") == 0);
  CHECK(combos("AA:1") == 6);
  CHECK(combos("AA:.5") == 6);
  CHECK(range("AA:1.0").front().weight == 1.0);
  CHECK(range("AA:.25").front().weight == 0.25);
  CHECK(range("AA:0.10").front().weight == 0.1);  // the double nearest to 0.1, as in C++ source
  CHECK(range("AA:0.333").front().weight == 0.333);
}

TEST_CASE("weights do not depend on the locale", "[range]") {
  // Some locales write decimals with a comma ("0,5"). A program that switches to one (GUI
  // toolkits call setlocale(LC_ALL, "")) must still read "AA:0.5" the same way.
  const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
  const char* comma_locale = nullptr;
  for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8"}) {
    if (std::setlocale(LC_NUMERIC, name) && std::localeconv()->decimal_point[0] == ',') {
      comma_locale = name;
      break;
    }
  }
  if (!comma_locale) {
    std::setlocale(LC_NUMERIC, previous.c_str());
    SKIP("no locale with a decimal comma is installed");
  }
  INFO("locale " << comma_locale);
  std::string error;
  const std::optional<Range> r = nuts::parse_range("AA:0.5, KK", &error);
  std::setlocale(LC_NUMERIC, previous.c_str());  // before any REQUIRE can end the test
  INFO(error);
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 12);
  CHECK(r->front().weight == 0.5);
}

TEST_CASE("empty text gives an empty range", "[range]") {
  CHECK(combos("") == 0);
  CHECK(combos(" , ,") == 0);
}

TEST_CASE("invalid ranges are rejected with a message", "[range]") {
  for (const char* bad : {"XX",   "AKx",     "AAs",    "AA+s",    "A2s-K7s", "AKs-AQo", "22-AKs",
                          "AsAs", "AsKsQs",  "A",      "+",       "AA:1.5",  "AA:-1",   "AA:abc",
                          "AA:",  "AA:0.5x", "AA:nan", "AA, AKz", "AK-",     "-AK",     "AKs+-",
                          "AA:.", "AA:0.5.5", "AA:1e-1", "AA:0.0000000000000000001"}) {
    std::string error;
    INFO("range: '" << bad << "'");
    CHECK_FALSE(nuts::parse_range(bad, &error).has_value());
    CHECK_FALSE(error.empty());
  }
  CHECK_FALSE(nuts::parse_range("QQ+, K", nullptr).has_value());  // error may be null
}
