#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "nuts/embedded_data.hpp"
#include "nuts/pushfold.hpp"

#ifndef NUTS_DATA_DIR
#error "NUTS_DATA_DIR must be defined (see CMakeLists.txt)"
#endif

// First in this file, so that (with Catch2's default order, the order of declaration) these
// threads are the ones that set the matrix up. Run under ThreadSanitizer in CI.
TEST_CASE("embedded equity matrix: first use from several threads at once",
          "[pushfold][embedded]") {
  std::vector<const nuts::Equity169*> results(4, nullptr);
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < results.size(); ++t) {
    threads.emplace_back([&results, t] { results[t] = nuts::embedded_equity169(); });
  }
  for (std::thread& thread : threads) thread.join();
  REQUIRE(results[0] != nullptr);
  for (const nuts::Equity169* result : results) CHECK(result == results[0]);
}

TEST_CASE("embedded equity matrix is the committed data file", "[pushfold][embedded]") {
  const std::string path = std::string(NUTS_DATA_DIR) + "/equity169.bin";
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file);
  const std::vector<std::uint8_t> committed((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());

  const std::span<const std::uint8_t> embedded = nuts::embedded_equity169_bytes();
  REQUIRE(embedded.size() == 228'504);
  REQUIRE(embedded.size() == committed.size());
  CHECK(std::equal(embedded.begin(), embedded.end(), committed.begin()));
}

TEST_CASE("embedded equity matrix parses, once", "[pushfold][embedded]") {
  std::string error;
  const nuts::Equity169* equity = nuts::embedded_equity169(&error);
  INFO(error);
  REQUIRE(equity != nullptr);
  CHECK(nuts::embedded_equity169() == equity);  // the same object every time

  // AA against KK: about 82% (card removal included), and the two sides add up to 1.
  const int aa = *nuts::parse_hand_class("AA");
  const int kk = *nuts::parse_hand_class("KK");
  CHECK(equity->equity(aa, kk) > 0.81);
  CHECK(equity->equity(aa, kk) < 0.83);
  CHECK_THAT(equity->equity(aa, kk) + equity->equity(kk, aa),
             Catch::Matchers::WithinAbs(1.0, 1e-15));
}
