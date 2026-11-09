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
#include "cli/CliError.h"
#include "cli/LoadMap.h"
#include "cli/Ops.h"
#include "cli/Targets.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::cli
{
namespace
{

// maps/targets.map: layer:0 holds brush:0, the brush entity entity:0 (func_door) and
// group:0 (id 5) with two brushes; layer:1 is locked and holds brush:0 and the point
// entity entity:0 (light); layer:2 is hidden and holds brush:0
constexpr auto TargetsMap = "maps/targets.map";

std::vector<std::string> paths(const std::vector<NodeOrFace>& nodes)
{
  auto result = std::vector<std::string>{};
  for (const auto& [node, faceIndex] : nodes)
  {
    result.push_back(
      faceIndex ? facePath(static_cast<const mdl::BrushNode&>(*node), *faceIndex)
                : nodePath(*node));
  }
  return result;
}

ErrorCode errorCode(const auto& f)
{
  try
  {
    f();
  }
  catch (const CliError& e)
  {
    return e.code();
  }
  FAIL("no CliError was thrown");
  return ErrorCode::InternalError;
}

} // namespace

TEST_CASE("Targets")
{
  auto logger = NullLogger{};
  auto loadedMap = loadFixtureMap(TargetsMap, logger);
  auto& map = *loadedMap.map;
  auto& worldNode = map.worldNode();

  SECTION("toString")
  {
    CHECK(toString(TargetKind::BrushEntity) == "brush_entity");
    CHECK(toString(TargetKind::Face) == "face");
    CHECK(toString(std::vector<TargetKind>{}) == "");
    CHECK(
      toString(std::vector{TargetKind::Layer, TargetKind::PointEntity})
      == "layer, point_entity");
  }

  SECTION("targetKindOf")
  {
    using T = std::tuple<std::string, TargetKind>;
    const auto [path, kind] = GENERATE(
      T{"world", TargetKind::World},
      T{"layer:0", TargetKind::Layer},
      T{"layer:0/group:0", TargetKind::Group},
      T{"layer:1/entity:0", TargetKind::PointEntity},
      T{"layer:0/entity:0", TargetKind::BrushEntity},
      T{"layer:0/brush:0", TargetKind::Brush},
      T{"layer:0/brush:0/face:2", TargetKind::Face});
    CAPTURE(path);
    CHECK(targetKindOf(resolveNodePath(worldNode, path)) == kind);
  }

  SECTION("isBindingName")
  {
    CHECK(isBindingName("$room"));
    CHECK(isBindingName("$_a1"));
    CHECK(!isBindingName("$"));
    CHECK(!isBindingName("$1room"));
    CHECK(!isBindingName("$ro-om"));
    CHECK(!isBindingName("room"));
  }

  SECTION("isPathOrBinding")
  {
    CHECK(isPathOrBinding(Json{"layer:0/brush:0"}));
    CHECK(isPathOrBinding(Json{"$box"}));
    CHECK(!isPathOrBinding(Json{""}));
    CHECK(!isPathOrBinding(Json{"$"}));
    CHECK(!isPathOrBinding(Json{3}));
    CHECK(!isPathOrBinding(parseJsonLiteral(R"(["layer:0"])")));
  }

  SECTION("isTarget")
  {
    CHECK(isTarget(Json{"layer:0/brush:0"}));
    CHECK(isTarget(Json{"$box"}));
    CHECK(isTarget(parseJsonLiteral(R"(["layer:0/brush:0", "$box"])")));
    CHECK(isTarget(parseJsonLiteral(R"({"query": "true"})")));
    CHECK(isTarget(parseJsonLiteral(R"({"query": "true", "expect": 0})")));
    CHECK(isTarget(parseJsonLiteral(R"({"query": "true", "expect": "any"})")));

    CHECK(!isTarget(Json{"$"}));
    CHECK(!isTarget(Json{3}));
    CHECK(!isTarget(Json::array()));
    CHECK(!isTarget(parseJsonLiteral(R"(["layer:0", ""])")));
    CHECK(!isTarget(parseJsonLiteral(R"({"query": "true", "expect": -2})")));
    CHECK(!isTarget(parseJsonLiteral(R"({"query": "true", "other": 1})")));
  }

  SECTION("bindingReferences")
  {
    CHECK(bindingReferences(Json{"layer:0"}).empty());
    CHECK(bindingReferences(Json{"$a"}) == std::vector<std::string>{"$a"});
    CHECK(
      bindingReferences(parseJsonLiteral(R"(["$a", "layer:0", "$b"])"))
      == std::vector<std::string>{"$a", "$b"});
    CHECK(bindingReferences(parseJsonLiteral(R"({"query": "true"})")).empty());
  }

  SECTION("queryTargetFromFlags")
  {
    CHECK(
      queryTargetFromFlags("op", "true", std::nullopt)
      == parseJsonLiteral(R"({"query": "true"})"));
    CHECK(
      queryTargetFromFlags("op", "true", "3")
      == parseJsonLiteral(R"({"query": "true", "expect": 3})"));
    CHECK(
      queryTargetFromFlags("op", "true", "any")
      == parseJsonLiteral(R"({"query": "true", "expect": "any"})"));
    CHECK(
      errorCode([]() { queryTargetFromFlags("op", std::nullopt, "3"); })
      == ErrorCode::InvalidArgument);
    CHECK(
      errorCode([]() { queryTargetFromFlags("op", "true", "-1"); })
      == ErrorCode::InvalidArgument);
  }

  SECTION("isInMap")
  {
    auto* brushNode = resolveNodePath(worldNode, "layer:0/brush:0").node;
    CHECK(isInMap(worldNode, worldNode));
    CHECK(isInMap(*brushNode, worldNode));

    mdl::removeNodes(map, {brushNode});
    CHECK(!isInMap(*brushNode, worldNode));
  }

  SECTION("nodeAndDescendants")
  {
    const auto pathsOf = [&](const std::vector<mdl::Node*>& nodes) {
      auto result = std::vector<std::string>{};
      for (const auto* node : nodes)
      {
        result.push_back(nodePath(*node));
      }
      return result;
    };

    auto* group = resolveNodePath(worldNode, "layer:0/group:0").node;
    CHECK(
      pathsOf(nodeAndDescendants(*group))
      == std::vector<std::string>{
        "layer:0/group:0", "layer:0/group:0/brush:0", "layer:0/group:0/brush:1"});

    auto* brush = resolveNodePath(worldNode, "layer:0/brush:0").node;
    CHECK(nodeAndDescendants(*brush) == std::vector<mdl::Node*>{brush});
  }

  SECTION("appendOnce")
  {
    auto* layer = resolveNodePath(worldNode, "layer:0").node;
    auto* brush = resolveNodePath(worldNode, "layer:0/brush:0").node;
    auto nodes = std::vector<mdl::Node*>{brush};
    appendOnce(nodes, layer);
    appendOnce(nodes, brush);
    CHECK(nodes == std::vector<mdl::Node*>{brush, layer});
  }

  SECTION("ancestorsOf")
  {
    auto* layer = resolveNodePath(worldNode, "layer:0").node;
    auto* group = resolveNodePath(worldNode, "layer:0/group:0").node;
    auto* groupBrush = resolveNodePath(worldNode, "layer:0/group:0/brush:0").node;
    auto* brush = resolveNodePath(worldNode, "layer:0/brush:0").node;
    CHECK(
      ancestorsOf({groupBrush, brush, group})
      == std::vector<mdl::Node*>{group, layer, &worldNode});
  }

  SECTION("appendRemovedNodes")
  {
    auto* layer = resolveNodePath(worldNode, "layer:0").node;
    auto* group = resolveNodePath(worldNode, "layer:0/group:0").node;
    mdl::removeNodes(map, {group});

    auto removed = std::vector<mdl::Node*>{group};
    appendRemovedNodes({group, layer, &worldNode}, worldNode, removed);
    CHECK(removed == std::vector<mdl::Node*>{group});

    auto* groupBrush = group->children().front();
    appendRemovedNodes({groupBrush, layer}, worldNode, removed);
    CHECK(removed == std::vector<mdl::Node*>{group, groupBrush});
  }

  SECTION("recordMoveOutcome")
  {
    auto* layer = resolveNodePath(worldNode, "layer:0").node;
    auto* entity = resolveNodePath(worldNode, "layer:0/entity:0").node;
    auto* brush = entity->children().front();
    const auto ancestors = ancestorsOf({brush});
    REQUIRE(mdl::reparentNodes(map, {{layer, {brush}}}));

    auto outcome = OpOutcome{};
    recordMoveOutcome({brush}, ancestors, worldNode, outcome);
    CHECK(outcome.modified == std::vector<mdl::Node*>{brush});
    CHECK(outcome.removed == std::vector<mdl::Node*>{entity});
  }
}

TEST_CASE("TargetResolver")
{
  auto logger = NullLogger{};
  auto loadedMap = loadFixtureMap(TargetsMap, logger);
  auto& map = *loadedMap.map;
  auto& worldNode = map.worldNode();

  const auto anyKind = TargetRules{{
    TargetKind::World,
    TargetKind::Layer,
    TargetKind::Group,
    TargetKind::PointEntity,
    TargetKind::BrushEntity,
    TargetKind::Brush,
    TargetKind::Patch,
    TargetKind::Face,
  }};
  const auto brushRules = TargetRules{{TargetKind::Brush}, true};

  SECTION("inputPath")
  {
    const auto targets = TargetResolver{map, false, nullptr};
    const auto brush = resolveNodePath(worldNode, "layer:0/group:0/brush:1");
    CHECK(targets.inputPath(*brush.node) == "layer:0/group:0/brush:1");

    // a node that was not in the input rev has no path in it
    const auto copy = brush.node->clone(map.worldBounds());
    CHECK(targets.inputPath(*copy) == std::nullopt);
  }

  SECTION("resolve")
  {
    SECTION("resolves paths up front and keeps bindings for later")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      const auto resolved =
        targets.resolve(parseJsonLiteral(R"(["layer:0/brush:0", "$box"])"));
      REQUIRE(resolved.size() == 2);
      CHECK(resolved[0].source == "layer:0/brush:0");
      REQUIRE(resolved[0].node);
      CHECK(resolved[0].node->node == resolveNodePath(worldNode, "layer:0/brush:0").node);
      CHECK(resolved[1].source == "$box");
      CHECK(!resolved[1].node);
    }

    SECTION("a path that addresses nothing fails")
    {
      CHECK(
        errorCode(
          [&]() { TargetResolver{map, false, nullptr}.resolve(Json{"layer:0/brush:9"}); })
        == ErrorCode::NodeNotFound);
    }

    SECTION("drops locked nodes from query matches before checking expect")
    {
      const auto query =
        parseJsonLiteral(R"({"query": "type is \"brush\"", "expect": 5})");
      CHECK(TargetResolver{map, false, nullptr}.resolve(query).size() == 5);
      CHECK(
        errorCode([&]() { TargetResolver{map, true, nullptr}.resolve(query); })
        == ErrorCode::ExpectMismatch);
    }
  }

  SECTION("bind")
  {
    auto targets = TargetResolver{map, false, nullptr};
    const auto resolved = targets.resolve(Json{"$pair"});
    auto* groupNode = resolveNodePath(worldNode, "layer:0/group:0").node;
    targets.bind("$pair", groupNode->children() | kdl::ranges::to<std::vector>());
    CHECK(
      paths(targets.nodes(resolved, anyKind, "op: target"))
      == std::vector<std::string>{"layer:0/group:0/brush:0", "layer:0/group:0/brush:1"});
  }

  SECTION("replace")
  {
    auto targets = TargetResolver{map, false, nullptr};
    const auto resolved = targets.resolve(Json{"layer:0/brush:0"});

    auto* layer = resolveNodePath(worldNode, "layer:0").node;
    auto* brush = resolveNodePath(worldNode, "layer:0/brush:0").node;
    auto* copy = mdl::addNode(map, *layer, brush->clone(map.worldBounds()));
    mdl::removeNodes(map, {brush});
    targets.replace({{brush, copy}});

    const auto nodes = targets.nodes(resolved, anyKind, "op: target");
    REQUIRE(nodes.size() == 1);
    CHECK(nodes.front().node == copy);
    CHECK(targets.inputPath(*copy) == "layer:0/brush:0");
  }

  SECTION("current")
  {
    auto targets = TargetResolver{map, false, nullptr};
    auto* brush = resolveNodePath(worldNode, "layer:0/brush:0").node;
    const auto copy = brush->clone(map.worldBounds());
    const auto copyOfCopy = brush->clone(map.worldBounds());
    CHECK(targets.current(brush) == brush);

    targets.replace({{brush, copy.get()}});
    targets.replace({{copy.get(), copyOfCopy.get()}});
    CHECK(targets.current(brush) == copyOfCopy.get());
    CHECK(targets.current(copyOfCopy.get()) == copyOfCopy.get());
  }

  SECTION("nodes")
  {
    SECTION("expands layers, groups and brush entities to their brushes if asked")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      const auto resolved = targets.resolve(
        parseJsonLiteral(R"(["layer:0/entity:0", "layer:0/group:0", "layer:2"])"));
      CHECK(
        paths(targets.nodes(resolved, brushRules, "op: target"))
        == std::vector<std::string>{
          "layer:0/entity:0/brush:0",
          "layer:0/group:0/brush:0",
          "layer:0/group:0/brush:1",
          "layer:2/brush:0",
        });
    }

    SECTION("lists each node once")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      const auto resolved =
        targets.resolve(parseJsonLiteral(R"(["layer:0/group:0/brush:1", "layer:0"])"));
      CHECK(
        paths(targets.nodes(resolved, brushRules, "op: target"))
        == std::vector<std::string>{
          "layer:0/group:0/brush:1",
          "layer:0/brush:0",
          "layer:0/entity:0/brush:0",
          "layer:0/group:0/brush:0",
        });
    }

    SECTION("does not expand unless asked")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      const auto resolved = targets.resolve(Json{"layer:0/group:0"});
      CHECK(
        errorCode([&]() {
          targets.nodes(resolved, TargetRules{{TargetKind::Brush}}, "op: target");
        })
        == ErrorCode::WrongNodeKind);
    }

    SECTION("a container without brushes cannot be expanded")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      auto* brushNode = resolveNodePath(worldNode, "layer:2/brush:0").node;
      const auto resolved = targets.resolve(Json{"layer:2"});
      mdl::removeNodes(map, {brushNode});
      CHECK(
        errorCode([&]() { targets.nodes(resolved, brushRules, "op: target"); })
        == ErrorCode::WrongNodeKind);
    }

    SECTION("a removed node fails")
    {
      const auto targets = TargetResolver{map, false, nullptr};
      auto* brushNode = resolveNodePath(worldNode, "layer:0/brush:0").node;
      const auto resolved = targets.resolve(Json{"layer:0/brush:0"});
      mdl::removeNodes(map, {brushNode});
      CHECK(
        errorCode([&]() { targets.nodes(resolved, anyKind, "op: target"); })
        == ErrorCode::TargetDeleted);
    }

    SECTION("an expanded container with locked brushes is locked")
    {
      const auto resolved = TargetResolver{map, false, nullptr}.resolve(Json{"layer:1"});
      CHECK(
        errorCode([&]() {
          TargetResolver{map, false, nullptr}.nodes(resolved, brushRules, "op: target");
        })
        == ErrorCode::Locked);
      CHECK(
        paths(
          TargetResolver{map, true, nullptr}.nodes(resolved, brushRules, "op: target"))
        == std::vector<std::string>{"layer:1/brush:0"});
    }

    SECTION("a locked node is accepted if the rules do not refuse locked nodes")
    {
      const auto resolved = TargetResolver{map, false, nullptr}.resolve(Json{"layer:1"});
      const auto rules = TargetRules{{TargetKind::Layer}, false, false};
      CHECK(
        paths(TargetResolver{map, false, nullptr}.nodes(resolved, rules, "op: target"))
        == std::vector<std::string>{"layer:1"});
    }
  }
}

} // namespace tb::cli
