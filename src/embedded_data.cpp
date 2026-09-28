// The parsed form of the compiled-in equity matrix. The bytes themselves are in a file that CMake
// generates from data/equity169.bin (see src/embedded_equity169_bytes.cpp.in).
#include "nuts/embedded_data.hpp"

#include <optional>

namespace nuts {

const Equity169* embedded_equity169(std::string* error) {
  // Parsed once, on the first call. The message is kept too, so that every call that fails
  // (they all fail the same way) can report it.
  struct Parsed {
    std::optional<Equity169> equity;
    std::string error;
  };
  static const Parsed parsed = [] {
    Parsed p;
    p.equity = equity169_from_bytes(embedded_equity169_bytes(), &p.error);
    return p;
  }();

  if (!parsed.equity) {
    if (error) *error = "the compiled-in equity matrix is invalid: " + parsed.error;
    return nullptr;
  }
  return &*parsed.equity;
}

}  // namespace nuts
