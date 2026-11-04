//
// Lir - The Distributed Event System
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "lir/version.h"

#include <gtest/gtest.h>

#include <string>

TEST(Version, LibraryMatchesHeaders) { EXPECT_EQ(std::string(lir::library_version()), std::string(lir::kVersion)); }

TEST(Version, IsSemver) {
  EXPECT_EQ(std::string(lir::kVersion),
            std::to_string(lir::kVersionMajor) + "." + std::to_string(lir::kVersionMinor) + "." + std::to_string(lir::kVersionPatch));
}
