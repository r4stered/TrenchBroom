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
#include "cli/CliValidators.h"
#include "cli/Config.h"
#include "cli/IssueTypes.h"
#include "cli/LoadMap.h"
#include "cli/NodePath.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
namespace
{

/*
 * maps/cli_checks.map has, besides a floor (brush:0) and a water pool (brush:1):
 *   world: worldtype 5, which is not a choice, and sounds "two", which is no integer
 *   layer:0/brush:2: a box with a corner cut off by a face of area 0.078
 *   layer:0/brush:3: a tetrahedron of volume 1/6
 *   layer:0/entity:0: an info_player_start2 inside the floor, with spawnflags 260, of
 *                     which 256 is the engine's "not on easy" and 4 has no flag
 *   layer:0/entity:1: a light inside the water pool
 *   layer:0/entity:2: an item_health on the floor
 *   layer:0/entity:4: an info_null inside the brush of a func_door (entity:3)
 *   layer:0/entity:5: an info_null inside a brush of the group group:0
 * and no player start.
 */
struct ValidatorFixture
{
  NullLogger logger;
  LoadedMap loadedMap;

  explicit ValidatorFixture(const std::filesystem::path& fixture = "maps/cli_checks.map")
    : loadedMap{loadFixtureMap(fixture, logger)}
  {
  }

  /** The messages of the issues that the validator finds on the node at the path. */
  std::vector<std::string> messages(
    const mdl::Validator& validator, const std::string_view path)
  {
    auto& node = *resolveNodePath(loadedMap.map->worldNode(), path).node;
    auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
    validator.validate(node, issues);

    auto result = std::vector<std::string>{};
    for (const auto& issue : issues)
    {
      CHECK(issue->type() == validator.type());
      CHECK(&issue->node() == &node);
      result.push_back(issue->description());
    }
    return result;
  }
};

using Messages = std::vector<std::string>;

} // namespace

TEST_CASE("MissingMaterialValidator")
{
  // without a Quake directory, no material is found
  auto fixture = ValidatorFixture{};
  const auto validator = MissingMaterialValidator{};

  CHECK(
    fixture.messages(validator, "layer:0/brush:1")
    == Messages(6, "Material '*water1' not found"));

  auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
  validator.validate(
    *resolveNodePath(fixture.loadedMap.map->worldNode(), "layer:0/brush:1").node, issues);
  REQUIRE(issues.size() == 6);
  const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issues[2].get());
  REQUIRE(faceIssue != nullptr);
  CHECK(faceIssue->faceIndex() == 2);

  CHECK(fixture.messages(validator, "layer:0/entity:0").empty());
}

TEST_CASE("FgdPropertyTypeValidator")
{
  auto fixture = ValidatorFixture{};
  const auto validator = FgdPropertyTypeValidator{};

  SECTION("choices and integers")
  {
    CHECK(
      fixture.messages(validator, "world")
      == Messages{
        "worldspawn property 'worldtype' value '5' is not one of the choices 0, 1, 2",
        "worldspawn property 'sounds' value 'two' is not an integer",
      });
  }

  SECTION("flags")
  {
    CHECK(
      fixture.messages(validator, "layer:0/entity:0")
      == Messages{"info_player_start2 property 'spawnflags' value '260' sets flags "
                  "without a definition (4)"});
  }

  SECTION("values that fit their definition, and properties without one")
  {
    CHECK(fixture.messages(validator, "layer:0/entity:1").empty());
  }

  SECTION("issues name their property")
  {
    auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
    validator.validate(fixture.loadedMap.map->worldNode(), issues);
    REQUIRE(issues.size() == 2);
    const auto* propertyIssue =
      dynamic_cast<const mdl::EntityPropertyIssue*>(issues[0].get());
    REQUIRE(propertyIssue != nullptr);
    CHECK(propertyIssue->propertyKey() == "worldtype");
  }
}

TEST_CASE("EntityInsideSolidValidator")
{
  auto fixture = ValidatorFixture{};
  const auto validator = EntityInsideSolidValidator{};

  SECTION("a point entity inside a world brush")
  {
    CHECK(
      fixture.messages(validator, "layer:0/entity:0")
      == Messages{"info_player_start2 origin is inside a world brush"});
  }

  SECTION("a point entity inside a brush in a group")
  {
    CHECK(
      fixture.messages(validator, "layer:0/entity:5")
      == Messages{"info_null origin is inside a world brush"});
  }

  SECTION("a point entity inside a liquid")
  {
    CHECK(fixture.messages(validator, "layer:0/entity:1").empty());
  }

  SECTION("a point entity on the surface of a world brush")
  {
    CHECK(fixture.messages(validator, "layer:0/entity:2").empty());
  }

  SECTION("a point entity inside the brush of an entity")
  {
    CHECK(fixture.messages(validator, "layer:0/entity:4").empty());
  }

  SECTION("a brush entity")
  {
    CHECK(fixture.messages(validator, "layer:0/entity:3").empty());
  }

  SECTION("a point entity inside a world brush that was added after loading")
  {
    auto& map = *fixture.loadedMap.map;
    auto& worldNode = map.worldNode();
    const auto builder = mdl::BrushBuilder{worldNode.mapFormat(), map.worldBounds()};
    // around the item_health at 0 64 0
    REQUIRE(
      mdl::addNode(
        map,
        *worldNode.defaultLayer(),
        std::make_unique<mdl::BrushNode>(
          builder.createCuboid(vm::bbox3d{{-16, 48, -16}, {16, 80, 16}}, "box")
            .value())));

    CHECK(
      fixture.messages(validator, "layer:0/entity:2")
      == Messages{"item_health origin is inside a world brush"});
  }
}

TEST_CASE("NoPlayerStartValidator")
{
  const auto validator = NoPlayerStartValidator{};

  SECTION("a map without a player start")
  {
    auto fixture = ValidatorFixture{};
    CHECK(
      fixture.messages(validator, "world")
      == Messages{"Map has no player start (info_player_start, "
                  "info_player_deathmatch, info_player_coop)"});
    CHECK(fixture.messages(validator, "layer:0/entity:0").empty());
  }

  SECTION("a map with a player start")
  {
    auto fixture = ValidatorFixture{"maps/verify.map"};
    CHECK(fixture.messages(validator, "world").empty());
  }

  SECTION("a map with only a deathmatch start")
  {
    auto fixture = ValidatorFixture{};
    auto& worldNode = fixture.loadedMap.map->worldNode();
    auto* entityNode =
      dynamic_cast<mdl::EntityNode*>(resolveNodePath(worldNode, "layer:0/entity:1").node);
    REQUIRE(entityNode != nullptr);
    auto entity = entityNode->entity();
    entity.setClassname("info_player_deathmatch");
    entityNode->setEntity(std::move(entity));

    CHECK(fixture.messages(validator, "world").empty());
  }
}

TEST_CASE("DegenerateBrushValidator")
{
  auto fixture = ValidatorFixture{};
  const auto validator = DegenerateBrushValidator{};

  SECTION("a brush with a tiny face")
  {
    CHECK(
      fixture.messages(validator, "layer:0/brush:2")
      == Messages{"Face 5 area 0.0779 is below 0.1"});
  }

  SECTION("a brush with a tiny volume")
  {
    CHECK(
      fixture.messages(validator, "layer:0/brush:3")
      == Messages{"Brush volume 0.167 is below 1"});
  }

  SECTION("regular brushes")
  {
    CHECK(fixture.messages(validator, "layer:0/brush:0").empty());
    CHECK(fixture.messages(validator, "layer:0/group:0/brush:0").empty());
  }
}

TEST_CASE("makeCliValidators")
{
  const auto typesOf = [](const auto& validators) {
    auto result = std::vector<std::string_view>{};
    for (const auto& validator : validators)
    {
      const auto* type = findIssueType(*validator);
      REQUIRE(type != nullptr);
      CHECK(type->source == IssueSource::CliCheck);
      result.push_back(type->id);
    }
    return result;
  };

  CHECK(
    typesOf(makeCliValidators(MaterialCheck::On))
    == std::vector<std::string_view>{
      "missing_material",
      "fgd_property_type",
      "entity_inside_solid",
      "no_player_start",
      "degenerate_brush",
    });
  CHECK(
    typesOf(makeCliValidators(MaterialCheck::Off))
    == std::vector<std::string_view>{
      "fgd_property_type",
      "entity_inside_solid",
      "no_player_start",
      "degenerate_brush",
    });
}

TEST_CASE("materialCheckOf")
{
  auto logger = NullLogger{};

  // without a Quake directory, there is no game path to load materials from
  CHECK(
    materialCheckOf(*loadFixtureMap("maps/simple.map", logger).map)
    == MaterialCheck::Off);

  auto config = Config{};
  config.gameConfig = quakeGameConfigPath();
  config.fgd = {quakeFgdPath()};
  config.quakeDir = texturesFixturePath();
  CHECK(
    materialCheckOf(*loadMap(fixturePath("maps/simple.map"), config, logger).map)
    == MaterialCheck::On);
}

TEST_CASE("isMaterialMissing")
{
  auto logger = NullLogger{};
  const auto faceOf =
    [](const LoadedMap& loaded, const std::string& path) -> const mdl::BrushFace& {
    const auto [node, faceIndex] = resolveNodePath(loaded.map->worldNode(), path);
    return static_cast<const mdl::BrushNode*>(node)->brush().face(*faceIndex);
  };

  SECTION("materials that cannot be checked are not missing")
  {
    const auto loaded = loadFixtureMap("maps/faces.map", logger);
    CHECK_FALSE(isMaterialMissing(*loaded.map, faceOf(loaded, "layer:0/brush:0/face:0")));
  }

  SECTION("materials that can be checked")
  {
    auto env = CliTestEnvironment{};
    const auto map = env.copyFixture("maps/faces.map", "faces.map");
    setUpMaterials(env);

    auto config = Config{};
    config.gameConfig = quakeGameConfigPath();
    config.fgd = {quakeFgdPath()};
    config.quakeDir = env.path("quake");
    const auto found = loadMap(map, config, logger);
    CHECK_FALSE(isMaterialMissing(*found.map, faceOf(found, "layer:0/brush:0/face:0")));

    const auto missing = loadMap(fixturePath("maps/simple.map"), config, logger);
    CHECK(isMaterialMissing(*missing.map, faceOf(missing, "layer:0/brush:0/face:0")));
  }
}

} // namespace tb::cli
