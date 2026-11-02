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
#include "cli/EntityJson.h"
#include "cli/Json.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityNode.h"
#include "mdl/LayerNode.h"
#include "mdl/MapFormat.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/WorldNode.h"

#include "kd/result.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::cli
{
namespace
{

mdl::EntityNode& addEntity(
  mdl::WorldNode& worldNode, std::vector<mdl::EntityProperty> properties)
{
  auto* entityNode = new mdl::EntityNode{mdl::Entity{std::move(properties)}};
  worldNode.defaultLayer()->addChild(entityNode);
  return *entityNode;
}

} // namespace

TEST_CASE("EntityJson")
{
  SECTION("entityToJson")
  {
    auto worldNode = mdl::WorldNode{
      mdl::EntityPropertyConfig{},
      mdl::Entity{{{"classname", "worldspawn"}, {"wad", "a.wad"}}},
      mdl::MapFormat::Valve};

    const auto definition = mdl::EntityDefinition{
      "monster",
      Color{},
      "",
      {mdl::PropertyDefinition{
        "spawnflags",
        mdl::PropertyValueTypes::Flags{{
          {1, "Ambush", ""},
          {2, "", ""},
        }},
        "",
        ""}},
      mdl::PointEntityDefinition{vm::bbox3d{8.0}, {}, {}},
    };

    SECTION("worldspawn has neither origin nor aabb")
    {
      CHECK(entityToJson(worldNode) == parseJsonLiteral(R"({
        "path": "world", "classname": "worldspawn",
        "properties": {"classname": "worldspawn", "wad": "a.wad"},
        "definition": "missing"})"));
    }

    SECTION("point entities have an origin")
    {
      auto& entityNode =
        addEntity(worldNode, {{"classname", "monster"}, {"origin", "1 2.5 3"}});
      entityNode.setDefinition(&definition);

      CHECK(entityToJson(entityNode) == parseJsonLiteral(R"({
        "path": "layer:0/entity:0", "classname": "monster", "origin": [1, 2.5, 3],
        "properties": {"classname": "monster", "origin": "1 2.5 3"},
        "definition": "found"})"));
    }

    SECTION("brush entities have an aabb")
    {
      auto& entityNode = addEntity(worldNode, {{"classname", "func_door"}});
      const auto builder = mdl::BrushBuilder{mdl::MapFormat::Valve, vm::bbox3d{8192.0}};
      entityNode.addChild(
        new mdl::BrushNode{builder.createCube(64.0, "material") | kdl::value()});

      CHECK(entityToJson(entityNode)["aabb"] == parseJsonLiteral(R"(
        {"min": [-32, -32, -32], "max": [32, 32, 32]})"));
      CHECK(entityToJson(entityNode).find("origin") == nullptr);
    }

    SECTION("spawnflags are decoded to the names the definition gives them")
    {
      auto& entityNode =
        addEntity(worldNode, {{"classname", "monster"}, {"spawnflags", "7"}});
      entityNode.setDefinition(&definition);

      // bits without a name are given by value
      CHECK(
        entityToJson(entityNode)["spawnflags"]
        == parseJsonLiteral(R"(["Ambush", 2, 4])"));
    }

    SECTION("spawnflags are given by value without a definition")
    {
      auto& entityNode =
        addEntity(worldNode, {{"classname", "unknown"}, {"spawnflags", "5"}});

      const auto json = entityToJson(entityNode);
      CHECK(json["definition"] == Json{"missing"});
      CHECK(json["spawnflags"] == parseJsonLiteral("[1, 4]"));
    }

    SECTION("unset, zero and malformed spawnflags are not decoded")
    {
      const auto spawnflags = GENERATE("0", "-1", "abc", "1x");
      auto& entityNode =
        addEntity(worldNode, {{"classname", "monster"}, {"spawnflags", spawnflags}});
      entityNode.setDefinition(&definition);

      const auto json = entityToJson(entityNode);
      CHECK(json["properties"]["spawnflags"] == Json{spawnflags});
      CHECK(json.find("spawnflags") == nullptr);
    }
  }
}

} // namespace tb::cli
