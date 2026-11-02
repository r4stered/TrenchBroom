/*
 Copyright (C) 2026 Drew Williams

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CliTestUtils.h"
#include "base/Logger.h"
#include "cli/LoadMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

TEST_CASE("LoadMap")
{
  SECTION("loadMap")
  {
    auto logger = NullLogger{};

    SECTION("a map that loads exactly has no load problems")
    {
      const auto fixture = GENERATE(
        "maps/simple.map", "maps/describe.map", "maps/query.map", "maps/targets.map");
      CAPTURE(fixture);
      CHECK(loadFixtureMap(fixture, logger).loadProblems.empty());
    }

    SECTION("records what the reader skipped")
    {
      // maps/lossy.map is maps/simple.map with a face whose plane points coincide
      const auto loadedMap = loadFixtureMap("maps/lossy.map", logger);
      REQUIRE(!loadedMap.loadProblems.empty());
      CHECK_THAT(loadedMap.loadProblems.front(), ContainsSubstring("Skipping face"));
    }
  }
}

} // namespace tb::cli
