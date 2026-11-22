//
// Lir - The Message Broker
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "lir/mqtt/wire.h"

// A target's seeds: the files of lir/fuzz/corpus/<target>/ (made by
// seeds.py), read where the build was configured, in name order.
namespace lir::fuzz {

inline std::vector<mqtt::Bytes> seeds(const std::string& target) {
  const std::filesystem::path directory = std::filesystem::path(LIR_FUZZ_CORPUS) / target;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file()) files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  std::vector<mqtt::Bytes> out;
  for (const auto& file : files) {
    std::ifstream in(file, std::ios::binary);
    out.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (out.empty()) throw std::runtime_error("no seeds in " + directory.string());
  return out;
}

}  // namespace lir::fuzz
