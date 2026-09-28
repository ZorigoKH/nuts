// The committed equity matrix, data/equity169.bin, compiled into the program.
//
// The Python module and the WebAssembly build solve push/fold on request, and the solver needs
// the 169 x 169 preflop equity matrix. Reading data/equity169.bin at run time would not work
// there: an installed Python package has no data/ directory next to it, and a web page has no
// file system. So the build turns the file into a C++ array instead (CMake target
// nuts::embedded_data; see CMakeLists.txt and src/embedded_equity169_bytes.cpp.in). The C++
// tests check that the array is byte-for-byte the committed file.
#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "nuts/pushfold.hpp"

namespace nuts {

// The bytes of data/equity169.bin as they were when the program was built (228,504 bytes).
std::span<const std::uint8_t> embedded_equity169_bytes() noexcept;

// The matrix read from those bytes with equity169_from_bytes(), which also checks them. The
// first call does the work (a millisecond or so); later calls return the same object. Safe to
// call from several threads (C++ guarantees that a function-local static is initialized once).
//
// Returns nullptr, with the reason in *error if error is not null, if the bytes are not a valid
// matrix file (which would mean a broken build).
const Equity169* embedded_equity169(std::string* error = nullptr);

}  // namespace nuts
