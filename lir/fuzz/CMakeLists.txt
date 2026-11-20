#
# Lir - The Message Broker
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One libFuzzer target per parser of untrusted bytes, <format>_fuzz.cpp, as
# Lugh has them; seeds in corpus/<format>/. Built only with LIR_BUILD_FUZZING
# and a clang that ships libFuzzer; the top level instruments lir::mqtt to
# match. tools/ci.sh fuzz runs them.
include(CheckCXXSourceCompiles)
# Compiled and linked: a plain flag check links a main() of its own, which
# libFuzzer's main collides with.
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_cxx_source_compiles("
#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
" LIR_HAS_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT LIR_HAS_LIBFUZZER)
  message(FATAL_ERROR "LIR_BUILD_FUZZING is on but ${CMAKE_CXX_COMPILER} has no libFuzzer (-fsanitize=fuzzer)")
endif()

foreach(format packet properties topic)
  add_executable(lir_fuzz_${format} ${format}_fuzz.cpp)
  target_link_libraries(lir_fuzz_${format} PRIVATE lir::mqtt lir_warnings)
  target_compile_options(lir_fuzz_${format} PRIVATE -fsanitize=fuzzer)
  target_link_options(lir_fuzz_${format} PRIVATE -fsanitize=fuzzer)
endforeach()
