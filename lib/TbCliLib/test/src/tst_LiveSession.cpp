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

#include "cli/LiveSession.h"

#include <optional>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("AgentHistory")
{
  auto history = AgentHistory{};

  SECTION("requestDone")
  {
    CHECK(history.mostRecentBatch("") == std::nullopt);

    history.requestDone("live-a:0", "live-a:2", 7);
    CHECK(history.mostRecentBatch("live-a:2") == 7);

    // a request that stored nothing keeps the batches
    history.requestDone("live-a:2", "live-a:3", std::nullopt);
    CHECK(history.mostRecentBatch("live-a:3") == 7);

    history.requestDone("live-a:3", "live-a:5", 9);
    CHECK(history.mostRecentBatch("live-a:5") == 9);

    // the map changed without the agent before this request
    history.requestDone("live-a:6", "live-a:8", 11);
    CHECK(history.mostRecentBatch("live-a:8") == 11);
    history.batchUndone("live-a:9");
    CHECK(history.mostRecentBatch("live-a:9") == std::nullopt);
  }

  SECTION("mostRecentBatch")
  {
    history.requestDone("", "live-a:2", 7);
    CHECK(history.mostRecentBatch("live-a:2") == 7);
    // the map changed without the agent since its last request
    CHECK(history.mostRecentBatch("live-a:3") == std::nullopt);
  }

  SECTION("batchUndone")
  {
    history.requestDone("", "live-a:2", 7);
    history.requestDone("live-a:2", "live-a:4", 9);

    history.batchUndone("live-a:5");
    CHECK(history.mostRecentBatch("live-a:5") == 7);
    history.batchUndone("live-a:6");
    CHECK(history.mostRecentBatch("live-a:6") == std::nullopt);
  }
}

} // namespace tb::cli
