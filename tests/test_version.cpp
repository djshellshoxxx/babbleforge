#include <catch2/catch_test_macros.hpp>
#include <cstring>

#include "core/Version.h"

TEST_CASE("versionString is non-empty", "[version]") {
  const char* version = bf::versionString();
  REQUIRE(version != nullptr);
  REQUIRE(std::strlen(version) > 0);
}
