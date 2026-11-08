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
#include "cli/Config.h"
#include "cli/IssueTracker.h"
#include "cli/IssueTypes.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "cli/Verify.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
namespace
{

/** Adds an axis-aligned box brush to the default layer of the map. */
mdl::BrushNode* addBox(mdl::Map& map, const vm::bbox3d& bounds)
{
  auto& worldNode = map.worldNode();
  const auto builder = mdl::BrushBuilder{worldNode.mapFormat(), map.worldBounds()};
  auto* brushNode = mdl::addNode(
    map,
    *worldNode.defaultLayer(),
    std::make_unique<mdl::BrushNode>(builder.createCuboid(bounds, "box").value()));
  REQUIRE(brushNode);
  return brushNode;
}

/** The type ids of the given issues. */
std::vector<std::string_view> typesOf(const std::vector<VerifyIssue>& issues)
{
  auto result = std::vector<std::string_view>{};
  for (const auto& issue : issues)
  {
    result.push_back(issue.type->id);
  }
  return result;
}

OpOutcome created(mdl::Node* node)
{
  return {.created = {node}};
}

OpOutcome modified(mdl::Node* node)
{
  return {.modified = {node}};
}

OpOutcome removed(mdl::Node* node)
{
  return {.removed = {node}};
}

} // namespace

TEST_CASE("IssueTracker")
{
  auto logger = NullLogger{};
  auto config = VerifyConfig{};

  SECTION("IssueTracker")
  {
    // remembers the issues of every node, so that checking all of them again finds
    // nothing new
    auto loadedMap = loadFixtureMap("maps/verify.map", logger);
    auto tracker = IssueTracker{loadedMap, config};
    CHECK(tracker.introduced({}).empty());
    CHECK(tracker.introduced(modified(&loadedMap.map->worldNode())).empty());
  }

  SECTION("introduced")
  {
    SECTION("every issue on a created node is new, and has no line")
    {
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      auto tracker = IssueTracker{loadedMap, config};

      auto* box = addBox(map, {{0.5, 0, 0}, {16, 16, 16}});
      const auto issues = tracker.introduced(created(box));
      CHECK(typesOf(issues) == std::vector<std::string_view>{"non_integer_vertices"});
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].node == box);
      CHECK(issues[0].line == std::nullopt);
      CHECK(issues[0].key.size() == 12);
    }

    SECTION("a copy that replaced a node had the node's issues before")
    {
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      auto* box = addBox(map, {{0.5, 0, 0}, {16, 16, 16}});
      auto tracker = IssueTracker{loadedMap, config};
      auto unaware = IssueTracker{loadedMap, config};

      auto* copy =
        mdl::addNode(map, *map.worldNode().defaultLayer(), box->clone(map.worldBounds()));
      REQUIRE(copy);
      mdl::removeNodes(map, {box});

      CHECK(tracker.introduced({.modified = {copy}, .replaced = {{box, copy}}}).empty());
      // without the replacement, the copy's issues would count as new
      CHECK(
        typesOf(unaware.introduced(modified(copy)))
        == std::vector<std::string_view>{"non_integer_vertices"});
    }

    SECTION("issues a modified node had before are not new")
    {
      auto loadedMap = loadFixtureMap("maps/verify.map", logger);
      auto& map = *loadedMap.map;
      auto tracker = IssueTracker{loadedMap, config};

      // the light has link_target and two empty_property_value issues
      auto& light = dynamic_cast<mdl::EntityNode&>(
        *resolveNodePath(map.worldNode(), "layer:0/entity:1").node);
      auto entity = light.entity();
      entity.removeProperty("message");
      light.setEntity(entity);
      CHECK(tracker.introduced(modified(&light)).empty());

      entity.addOrUpdateProperty("speed", "");
      light.setEntity(entity);
      const auto issues = tracker.introduced(modified(&light));
      CHECK(typesOf(issues) == std::vector<std::string_view>{"empty_property_value"});
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].propertyKey == "speed");
    }

    SECTION("a point entity inside a created or removed brush is checked")
    {
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      auto tracker = IssueTracker{loadedMap, config};

      // the player start is at 0 80 24
      auto* box = addBox(map, {{-16, 64, 0}, {16, 96, 48}});
      const auto issues = tracker.introduced(created(box));
      CHECK(typesOf(issues) == std::vector<std::string_view>{"entity_inside_solid"});
      REQUIRE(issues.size() == 1);
      CHECK(nodePath(*issues[0].node) == "layer:0/entity:0");

      // once the brush is gone, the entity is free again, so a new brush around it
      // introduces the issue again
      mdl::removeNodes(map, {box});
      CHECK(tracker.introduced(removed(box)).empty());
      auto* otherBox = addBox(map, {{-8, 72, 16}, {8, 88, 32}});
      CHECK(
        typesOf(tracker.introduced(created(otherBox)))
        == std::vector<std::string_view>{"entity_inside_solid"});
    }

    SECTION("an entity whose link target was removed is checked")
    {
      // in maps/simple.map, nothing targets the door, so link it from a new entity
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      auto* light = resolveNodePath(map.worldNode(), "layer:0/entity:2").node;
      mdl::selectNodes(map, {light});
      mdl::setEntityProperty(map, "target", "door1");
      mdl::deselectAll(map);
      auto tracker = IssueTracker{loadedMap, config};

      auto* door = resolveNodePath(map.worldNode(), "layer:0/entity:1").node;
      mdl::removeNodes(map, {door});
      const auto issues = tracker.introduced(removed(door));
      CHECK(typesOf(issues) == std::vector<std::string_view>{"link_target"});
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].node == light);
    }

    SECTION("map-wide issues are left out")
    {
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      auto tracker = IssueTracker{loadedMap, config};

      auto* playerStart = resolveNodePath(map.worldNode(), "layer:0/entity:0").node;
      mdl::removeNodes(map, {playerStart});
      CHECK(tracker.introduced(removed(playerStart)).empty());
    }

    SECTION("the configuration's severities and suppressions apply")
    {
      auto loadedMap = loadFixtureMap("maps/simple.map", logger);
      auto& map = *loadedMap.map;
      const auto* type = findIssueType("non_integer_vertices");
      config.severity[type] = Severity::Error;
      auto tracker = IssueTracker{loadedMap, config};

      auto* box = addBox(map, {{0.5, 0, 0}, {16, 16, 16}});
      const auto issues = tracker.introduced(created(box));
      REQUIRE(issues.size() == 1);
      CHECK(issues[0].severity == Severity::Error);

      config.ignore.push_back({type, std::nullopt, std::nullopt});
      auto* otherBox = addBox(map, {{0.5, 32, 0}, {16, 48, 16}});
      CHECK(tracker.introduced(created(otherBox)).empty());
    }
  }
}

} // namespace tb::cli
