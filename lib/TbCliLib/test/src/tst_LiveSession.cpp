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
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/TestFactory.h"
#include "mdl/WorldNode.h"

#include <memory>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
namespace
{

mdl::Node* addNode(mdl::Map& map, mdl::Node& parent, std::unique_ptr<mdl::Node> node)
{
  auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
  nodes.push_back(std::move(node));
  return mdl::addNodes(map, parent, std::move(nodes)).front();
}

mdl::Node* addBrush(mdl::Map& map, mdl::Node& parent)
{
  return addNode(map, parent, mdl::createBrushNode(map));
}

/** The changes as (serial number, kind, origin, via). */
std::vector<std::tuple<uint64_t, ChangeKind, ChangeOrigin, ChangeVia>> summarize(
  const std::vector<Change>& changes)
{
  auto result = std::vector<std::tuple<uint64_t, ChangeKind, ChangeOrigin, ChangeVia>>{};
  for (const auto& change : changes)
  {
    result.emplace_back(change.serialNumber, change.kind, change.origin, change.via);
  }
  return result;
}

} // namespace

TEST_CASE("ChangeLog")
{
  using T = std::tuple<uint64_t, ChangeKind, ChangeOrigin, ChangeVia>;

  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create();
  auto& layer = *map.worldNode().defaultLayer();
  auto* brush0 = addBrush(map, layer);
  auto log = ChangeLog{map, 3};

  SECTION("counter")
  {
    CHECK(log.counter() == 0);
    addBrush(map, layer);
    CHECK(log.counter() == 1);

    // selecting, hiding and showing don't count
    mdl::selectNodes(map, {brush0});
    mdl::deselectAll(map);
    mdl::hideNodes(map, {brush0});
    mdl::showAllNodes(map);
    CHECK(log.counter() == 1);

    map.undoCommand();
    map.undoCommand();
    map.undoCommand();
    map.undoCommand();
    map.undoCommand();
    CHECK(log.counter() == 2);
  }

  SECTION("covers")
  {
    CHECK(log.covers(0));
    for (auto i = 0; i < 4; ++i)
    {
      addBrush(map, layer);
    }
    // the first change made room for the last one
    CHECK(!log.covers(0));
    CHECK(log.covers(1));
    CHECK(log.covers(4));
  }

  SECTION("changesSince")
  {
    auto* brush1 = addBrush(map, layer);
    const auto serial = brush1->serialNumber();
    mdl::removeNodes(map, {brush1});
    map.undoCommand();
    map.redoCommand();

    // the first change made room for the last one
    CHECK(
      summarize(log.changesSince(0))
      == std::vector<T>{
        {serial, ChangeKind::Removed, ChangeOrigin::Mapper, ChangeVia::Do},
        {serial, ChangeKind::Created, ChangeOrigin::Mapper, ChangeVia::Undo},
        {serial, ChangeKind::Removed, ChangeOrigin::Mapper, ChangeVia::Redo},
      });
    CHECK(log.changesSince(0).back().counter == 4);
    CHECK(log.changesSince(3).size() == 1);
    CHECK(log.changesSince(4).empty());
  }

  SECTION("keeps a node's latest modification of a run of them")
  {
    mdl::selectNodes(map, {brush0});
    for (auto i = 0; i < 4; ++i)
    {
      REQUIRE(mdl::translateSelection(map, {0, 0, 8}));
    }

    const auto changes = log.changesSince(0);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].serialNumber == brush0->serialNumber());
    CHECK(changes[0].counter == log.counter());
    CHECK(log.covers(0));
    CHECK(log.changedSince(log.counter() - 1).contains(brush0->serialNumber()));
  }

  SECTION("changedSince")
  {
    auto* group = addNode(map, layer, std::make_unique<mdl::GroupNode>(mdl::Group{"g"}));
    auto* member = addBrush(map, *group);
    const auto since = log.counter();
    mdl::removeNodes(map, {group});

    // the nodes below a removed node left the map with it
    CHECK(
      log.changedSince(since)
      == std::unordered_set<uint64_t>{group->serialNumber(), member->serialNumber()});
    CHECK(log.changedSince(log.counter()).empty());
  }

  SECTION("AgentChanges")
  {
    const auto mapperBrush = addBrush(map, layer)->serialNumber();

    SECTION("marks the changes as the agent's")
    {
      auto* agentBrush = [&]() {
        const auto agentChanges = ChangeLog::AgentChanges{log};
        return addBrush(map, layer);
      }();
      addBrush(map, layer);

      const auto changes = log.changesSince(0);
      REQUIRE(changes.size() == 3);
      CHECK(changes[0].origin == ChangeOrigin::Mapper);
      CHECK(changes[1].serialNumber == agentBrush->serialNumber());
      CHECK(changes[1].origin == ChangeOrigin::Agent);
      CHECK(changes[2].origin == ChangeOrigin::Mapper);
    }

    SECTION("discard takes the agent's changes out, and keeps the older ones")
    {
      {
        auto agentChanges = ChangeLog::AgentChanges{log};
        for (auto i = 0; i < 4; ++i)
        {
          addBrush(map, layer);
        }
        agentChanges.discard();
      }

      CHECK(log.covers(0));
      CHECK(
        summarize(log.changesSince(0))
        == std::vector<T>{
          {mapperBrush, ChangeKind::Created, ChangeOrigin::Mapper, ChangeVia::Do}});
    }
  }
}

TEST_CASE("LiveSession")
{
  SECTION("parseLiveRev")
  {
    const auto parsed = parseLiveRev("live-ab12:7");
    REQUIRE(parsed.has_value());
    CHECK(parsed->epoch == "ab12");
    CHECK(parsed->counter == 7);

    for (const auto* invalid :
         {"", "live-ab12", "live-:7", "live-ab12:", "live-ab12:x", "ab12:7", "0123abcd"})
    {
      CAPTURE(invalid);
      CHECK(parseLiveRev(invalid) == std::nullopt);
    }
  }

  SECTION("formatLiveRev")
  {
    CHECK(formatLiveRev("ab12", 7) == "live-ab12:7");
  }

  SECTION("toString")
  {
    CHECK(toString(ChangeKind::Created) == "created");
    CHECK(toString(ChangeKind::Modified) == "modified");
    CHECK(toString(ChangeKind::Removed) == "removed");
    CHECK(toString(ChangeOrigin::Mapper) == "mapper");
    CHECK(toString(ChangeOrigin::Agent) == "agent");
    CHECK(toString(Touch::Changed) == "changed");
    CHECK(toString(Touch::AncestorChanged) == "ancestor_changed");
    CHECK(toString(Touch::DescendantChanged) == "descendant_changed");
  }

  SECTION("howTouched")
  {
    auto fixture = mdl::MapFixture{};
    auto& map = fixture.create();
    auto& world = map.worldNode();
    auto& layer = *world.defaultLayer();
    auto* group = addNode(map, layer, std::make_unique<mdl::GroupNode>(mdl::Group{"g"}));
    auto* member0 = addBrush(map, *group);
    auto* member1 = addBrush(map, *group);
    auto* entity = addNode(map, layer, std::make_unique<mdl::EntityNode>(mdl::Entity{}));
    auto* entityBrush = addBrush(map, *entity);

    const auto touched = [](const mdl::Node* node) {
      return std::unordered_set<uint64_t>{node->serialNumber()};
    };

    CHECK(howTouched(*member0, {}) == std::nullopt);
    CHECK(howTouched(*member0, touched(member0)) == Touch::Changed);
    CHECK(howTouched(*member0, touched(group)) == Touch::AncestorChanged);
    CHECK(howTouched(*member0, touched(&layer)) == Touch::AncestorChanged);
    // not by a sibling, nor by the world
    CHECK(howTouched(*member0, touched(member1)) == std::nullopt);
    CHECK(howTouched(*member0, touched(&world)) == std::nullopt);

    // groups and entities are touched by what is below them, layers are not
    CHECK(howTouched(*group, touched(member1)) == Touch::DescendantChanged);
    CHECK(howTouched(*entity, touched(entityBrush)) == Touch::DescendantChanged);
    CHECK(howTouched(layer, touched(member1)) == std::nullopt);
    CHECK(howTouched(world, touched(member1)) == std::nullopt);
  }
}

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
