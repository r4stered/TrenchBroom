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
#include "cli/MapRef.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("MapRef")
{
  SECTION("mapName")
  {
    auto logger = NullLogger{};
    auto loadedMap = loadFixtureMap("maps/simple.map", logger);
    CHECK(mapName(loadedMap.ref()) == "simple");
  }
}

} // namespace tb::cli
