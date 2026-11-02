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
#include "cli/FaceOps.h"
#include "cli/NodePath.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include "vm/vec.h"

#include <fmt/format.h>

#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

/*
 * maps/faces.map has three world brushes with the material cr8_czg_1 (64x128) on every
 * face, whose faces are sorted -x, +z, -y, -z, +y, +x once loaded, except brush:0's:
 *
 * brush:0  0 0 0 to 64 64 32, an axial box (faces -x, -y, -z, +z, +y, +x)
 * brush:1  128 0 0 to 384 64 33, whose top (face:1) slopes by 0.22 degrees
 * brush:2  512 0 0 to 640 64 35, whose top (face:1) slopes by 1.34 degrees
 */

namespace
{

/** maps/faces.map, with its materials loaded. */
struct FaceFixture : EditFixture
{
  FaceFixture()
    : EditFixture{"maps/faces.map"}
  {
    setUpMaterials(env);
  }
};

/** The face at the given path, as show reports it. */
Json showFace(const std::string& map, const std::string& path)
{
  return runJson({"show", map, path})["node"];
}

/** The materials of the faces of the brush at the given path, in face order. */
std::vector<std::string> materialsOf(const std::string& map, const std::string& path)
{
  const auto brush = runJson({"show", map, path})["node"];
  auto result = std::vector<std::string>{};
  for (const auto& face : brush["faces"].asArray())
  {
    result.push_back(face["texture"].asString());
  }
  return result;
}

using Materials = std::vector<std::string>;

} // namespace

TEST_CASE("FaceOps")
{
  SECTION("parseFaceSelector")
  {
    using Directions = std::vector<vm::vec3d>;
    CHECK(parseFaceSelector(Json{"all"})->directions == std::nullopt);
    CHECK(parseFaceSelector(Json{"-z"})->directions == Directions{{0, 0, -1}});
    CHECK(
      parseFaceSelector(parseJsonLiteral(R"(["+x", "-y"])"))->directions
      == Directions{{1, 0, 0}, {0, -1, 0}});
    CHECK(parseFaceSelector(Json{"up"}) == std::nullopt);
  }

  SECTION("isFaceSelector")
  {
    CHECK(isFaceSelector(Json{"all"}));
    CHECK(isFaceSelector(Json{"-z"}));
    CHECK(isFaceSelector(parseJsonLiteral(R"(["+x", "-y"])")));

    CHECK_FALSE(isFaceSelector(Json{"z"}));
    CHECK_FALSE(isFaceSelector(Json{"up"}));
    CHECK_FALSE(isFaceSelector(parseJsonLiteral("[]")));
    CHECK_FALSE(isFaceSelector(parseJsonLiteral(R"(["all"])")));
    CHECK_FALSE(isFaceSelector(parseJsonLiteral(R"(["+x", 1])")));
    CHECK_FALSE(isFaceSelector(Json{1}));
  }

  SECTION("selectFaces")
  {
    auto logger = NullLogger{};
    auto loaded = loadFixtureMap("maps/faces.map", logger);
    auto& worldNode = loaded.map->worldNode();
    const auto resolve = [&](const std::string& path) {
      return resolveNodePath(worldNode, path);
    };
    const auto paths = [](const std::vector<mdl::BrushFaceHandle>& faces) {
      auto result = std::vector<std::string>{};
      for (const auto& face : faces)
      {
        result.push_back(facePath(face.node(), face.faceIndex()));
      }
      return result;
    };
    using Paths = std::vector<std::string>;

    const auto brushes = std::vector<NodeOrFace>{
      resolve("layer:0/brush:0"), resolve("layer:0/brush:1"), resolve("layer:0/brush:2")};

    // normals within 0.5 degrees of the direction
    CHECK(
      paths(selectFaces(brushes, *parseFaceSelector(Json{"+z"})))
      == Paths{"layer:0/brush:0/face:3", "layer:0/brush:1/face:1"});
    CHECK(
      paths(selectFaces(
        {brushes[0]}, *parseFaceSelector(parseJsonLiteral(R"(["+x", "-x"])"))))
      == Paths{"layer:0/brush:0/face:0", "layer:0/brush:0/face:5"});
    CHECK(selectFaces({brushes[0]}, FaceSelector{}).size() == 6);

    // face paths are taken as they are, and each face is picked once
    CHECK(
      paths(selectFaces(
        {resolve("layer:0/brush:2/face:1"),
         brushes[0],
         resolve("layer:0/brush:0/face:3")},
        *parseFaceSelector(Json{"+z"})))
      == Paths{"layer:0/brush:2/face:1", "layer:0/brush:0/face:3"});
  }
}

TEST_CASE("set_material")
{
  auto fixture = FaceFixture{};
  const auto& map = fixture.map;
  const auto czg = std::string{"cr8_czg_1"};

  SECTION("sets the material of the faces that face a direction")
  {
    const auto json = runJson(
      {"set_material",
       map,
       "--target",
       "layer:0",
       "--faces",
       "+z",
       "--material",
       "added"});
    CHECK(json["ops"][0] == parseJsonLiteral(R"({
      "ok": true,
      "created": [],
      "modified": ["layer:0/brush:0", "layer:0/brush:1"],
      "removed": [],
      "warnings": [],
      "issues_introduced": []
    })"));
    CHECK(
      materialsOf(map, "layer:0/brush:0") == Materials{czg, czg, czg, "added", czg, czg});
    CHECK(
      materialsOf(map, "layer:0/brush:1") == Materials{czg, "added", czg, czg, czg, czg});
    CHECK(materialsOf(map, "layer:0/brush:2") == Materials(6, czg));
  }

  SECTION("sets the material of every face by default, and of face paths")
  {
    const auto json = parseOutput(fixture.apply(R"([{
      "op": "set_material", "target": ["layer:0/brush:0", "layer:0/brush:2/face:3"],
      "material": "added"
    }])"));
    CHECK(
      json["ops"][0]["modified"]
      == parseJsonLiteral(R"(["layer:0/brush:0", "layer:0/brush:2"])"));
    CHECK(materialsOf(map, "layer:0/brush:0") == Materials(6, "added"));
    CHECK(
      materialsOf(map, "layer:0/brush:2") == Materials{czg, czg, czg, "added", czg, czg});
  }

  SECTION("a material that is not found is a warning")
  {
    const auto json = runJson(
      {"set_material",
       map,
       "--target",
       "layer:0/brush:0/face:3",
       "--material",
       "no_such_material"});
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["missing_material"])"));
    CHECK(json["ops"][0]["issues_introduced"][0]["type"] == Json{"missing_material"});
    CHECK(materialsOf(map, "layer:0/brush:0")[3] == "no_such_material");
  }

  SECTION("without a Quake directory, materials cannot be checked")
  {
    fixture.env.writeConfig({}, quakeConfig());
    const auto json = runJson(
      {"set_material",
       map,
       "--target",
       "layer:0/brush:0/face:3",
       "--material",
       "no_such_material"});
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral("[]"));
  }

  SECTION("warns if the selector picks no face")
  {
    const auto json = runJson(
      {"set_material",
       map,
       "--target",
       "layer:0/brush:2",
       "--faces",
       "+z",
       "--material",
       "added"});
    CHECK(json["ops"][0]["warnings"] == parseJsonLiteral(R"(["no_faces_selected"])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("a face that has the material already is not modified")
  {
    const auto json =
      runJson({"set_material", map, "--target", "layer:0/brush:0", "--material", czg});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral("[]"));
  }

  SECTION("acts on brushes in groups and hidden layers")
  {
    // maps/targets.map: group:0 in layer:0 holds two crate brushes, layer:2 is hidden
    auto targets = EditFixture{"maps/targets.map"};

    const auto json = parseOutput(targets.apply(R"([{
      "op": "set_material", "target": ["layer:0/group:0", "layer:2"], "faces": "+z",
      "material": "top"
    }])"));
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"([
        "layer:0/group:0/brush:0", "layer:0/group:0/brush:1", "layer:2/brush:0"
      ])"));
    CHECK(materialsOf(targets.map, "layer:0/group:0/brush:1")[3] == "top");
    CHECK(materialsOf(targets.map, "layer:2/brush:0")[3] == "top");
  }

  SECTION("refuses faces together with a face path")
  {
    checkError(
      fixture.apply(R"([{
        "op": "set_material", "target": ["layer:0/brush:0", "layer:0/brush:1/face:0"],
        "faces": "+z", "material": "added"
      }])"),
      "invalid_argument",
      2,
      0);
    CHECK(fixture.unchanged());
  }

  SECTION("refuses an empty material, a malformed selector and entities")
  {
    checkError(
      runCli({"set_material", map, "--target", "layer:0/brush:0", "--material", ""}),
      "invalid_argument",
      2);
    checkError(
      runCli(
        {"set_material",
         map,
         "--target",
         "layer:0/brush:0",
         "--faces",
         "up",
         "--material",
         "added"}),
      "invalid_argument",
      2);
    checkError(
      runCli({"set_material", map, "--target", "world", "--material", "added"}),
      "wrong_node_kind",
      1,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("set_uv")
{
  auto fixture = FaceFixture{};
  const auto& map = fixture.map;
  const auto face = std::string{"layer:0/brush:0/face:3"};

  SECTION("sets values")
  {
    const auto json = runJson(
      {"set_uv",
       map,
       "--target",
       face,
       "--offset",
       "8,-4",
       "--scale",
       "0.5,2",
       "--rotation",
       "90"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));

    const auto uv = showFace(map, face)["uv"];
    CHECK(uv["offset"] == parseJsonLiteral("[8, -4]"));
    CHECK(uv["scale"] == parseJsonLiteral("[0.5, 2]"));
    CHECK(uv["rotation"] == Json{90});
  }

  SECTION("adds to values")
  {
    runJson({"set_uv", map, "--target", face, "--offset", "8,-4", "--rotation", "10"});
    const auto json = parseOutput(fixture.apply(
      fmt::format(
        R"([{{
        "op": "set_uv", "target": "{}", "offset": {{"add": [8, 4]}},
        "scale": {{"add": [1, 0.5]}}, "rotation": {{"add": 5}}
      }}])",
        face)));
    CHECK(json["ops"][0]["ok"] == Json{true});

    const auto uv = showFace(map, face)["uv"];
    CHECK(uv["offset"] == parseJsonLiteral("[16, 0]"));
    CHECK(uv["scale"] == parseJsonLiteral("[2, 1.5]"));
    CHECK(uv["rotation"] == Json{15});
  }

  SECTION("one number stands for both values of a pair")
  {
    runJson({"set_uv", map, "--target", face, "--scale", "2", "--offset", "4"});
    runJson({"set_uv", map, "--target", face, "--scale", R"({"add": 0.5})"});

    const auto uv = showFace(map, face)["uv"];
    CHECK(uv["offset"] == parseJsonLiteral("[4, 4]"));
    CHECK(uv["scale"] == parseJsonLiteral("[2.5, 2.5]"));
  }

  SECTION("the subcommand takes an addition as JSON")
  {
    runJson({"set_uv", map, "--target", face, "--rotation", R"({"add": 15})"});
    CHECK(showFace(map, face)["uv"]["rotation"] == Json{15});
  }

  SECTION("refuses a scale of 0")
  {
    checkError(
      runCli({"set_uv", map, "--target", face, "--scale", "0,1"}), "invalid_argument", 2);
    checkError(
      runCli({"set_uv", map, "--target", face, "--scale", "0"}), "invalid_argument", 2);

    const auto result =
      runCli({"set_uv", map, "--target", face, "--scale", R"({"add": [-1, 0]})"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{face});
    CHECK(fixture.unchanged());
  }

  SECTION("refuses malformed values")
  {
    const auto args = GENERATE(
      R"("faces": "+z")",
      R"("offset": [1, 2, 3])",
      R"("offset": {"add": "1"})",
      R"("rotation": {"add": 1, "x": 2})",
      R"("rotation": "90")");
    CAPTURE(args);
    checkError(
      fixture.apply(
        fmt::format(R"([{{"op": "set_uv", "target": "layer:0/brush:0", {}}}])", args)),
      "invalid_argument",
      2,
      0);
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("fit_uv")
{
  auto fixture = FaceFixture{};
  const auto& map = fixture.map;

  SECTION("fits the material to each face once")
  {
    // the -y face is 64 wide and 32 high, the material 64x128
    const auto json =
      runJson({"fit_uv", map, "--target", "layer:0/brush:0", "--faces", "-y"});
    CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));
    CHECK(
      showFace(map, "layer:0/brush:0/face:1")["uv"]["scale"]
      == parseJsonLiteral("[1, 0.25]"));
  }

  SECTION("fails if a material is not loaded")
  {
    fixture.env.writeConfig({}, quakeConfig());
    const auto result =
      runCli({"fit_uv", map, "--target", "layer:0/brush:0", "--faces", "-y"});
    checkError(result, "op_failed", 1, 0);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:0/face:1"});
    CHECK(fixture.unchanged());
  }
}

TEST_CASE("reset_uv")
{
  auto fixture = FaceFixture{};
  const auto& map = fixture.map;
  const auto face = std::string{"layer:0/brush:0/face:3"};

  runJson(
    {"set_uv",
     map,
     "--target",
     face,
     "--offset",
     "8,-4",
     "--scale",
     "0.5,2",
     "--rotation",
     "90"});

  const auto json = runJson({"reset_uv", map, "--target", "layer:0/brush:0"});
  CHECK(json["ops"][0]["modified"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));

  const auto uv = showFace(map, face)["uv"];
  CHECK(uv["offset"] == parseJsonLiteral("[0, 0]"));
  CHECK(uv["scale"] == parseJsonLiteral("[1, 1]"));
  CHECK(uv["rotation"] == Json{0});

  // a brush whose faces have the defaults already is not modified
  CHECK(
    runJson({"reset_uv", map, "--target", "layer:0/brush:0"})["ops"][0]["modified"]
    == parseJsonLiteral("[]"));
}

} // namespace tb::cli
