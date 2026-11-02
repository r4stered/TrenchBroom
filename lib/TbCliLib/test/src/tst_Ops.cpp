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
#include "cli/CliError.h"
#include "cli/IssueTypes.h"
#include "cli/Ops.h"

#include <fmt/format.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

TEST_CASE("ops command")
{
  SECTION("lists a JSON Schema per op")
  {
    const auto json = runJson({"ops", "--json"});
    CHECK(json["schema"] == Json{"tbcli.ops/1"});
    REQUIRE(json["ops"].size() == 38);
    CHECK(json["ops"][0]["name"] == Json{"create_box"});
    const auto opNamed = [&](const std::string_view name) -> const Json& {
      const auto& ops = json["ops"].asArray();
      const auto it = std::ranges::find_if(
        ops, [&](const auto& op) { return op["name"] == Json{name}; });
      REQUIRE(it != ops.end());
      return *it;
    };
    CHECK(opNamed("delete") == parseJsonLiteral(R"json({
      "name": "delete",
      "description": "Deletes groups, entities, brushes and patches. An entity or group left empty is deleted with them.",
      "schema": {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "type": "object",
        "properties": {
          "op": {"const": "delete"},
          "target": {
            "oneOf": [
              {"type": "string", "pattern": "^([^$]|\\$[A-Za-z_][A-Za-z0-9_]*$)"},
              {
                "type": "array",
                "items": {"type": "string", "pattern": "^([^$]|\\$[A-Za-z_][A-Za-z0-9_]*$)"},
                "minItems": 1
              },
              {
                "type": "object",
                "properties": {
                  "query": {"type": "string"},
                  "expect": {
                    "oneOf": [{"type": "integer", "minimum": 0}, {"const": "any"}],
                    "default": "any"
                  }
                },
                "required": ["query"],
                "additionalProperties": false
              }
            ],
            "description": "the nodes to delete",
            "x-node-kinds": ["group", "point_entity", "brush_entity", "brush", "patch"],
            "x-expands-to-brushes": false
          }
        },
        "required": ["op", "target"],
        "additionalProperties": false
      }
    })json"));

    const auto& createBox = json["ops"][0]["schema"];
    CHECK(
      createBox["required"] == parseJsonLiteral(R"(["op", "min", "max", "material"])"));
    CHECK(createBox["properties"]["min"]["type"] == Json{"array"});
    CHECK(createBox["properties"]["min"]["minItems"] == Json{3});
    CHECK(createBox["properties"]["parent"]["default"] == Json{"layer:0"});
    CHECK(
      createBox["properties"]["parent"]["x-node-kinds"]
      == parseJsonLiteral(R"(["layer", "group", "brush_entity"])"));
    CHECK(createBox["properties"]["as"]["pattern"].isString());

    const auto& createBrush = opNamed("create_brush")["schema"]["properties"];
    CHECK(createBrush["vertices"]["minItems"] == Json{4});
    CHECK(createBrush["vertices"]["items"]["minItems"] == Json{3});
    CHECK(
      opNamed("create_shape")["schema"]["properties"]["kind"]["enum"]
      == parseJsonLiteral(R"(["cylinder", "cone", "arch"])"));
    CHECK(
      opNamed("subtract")["schema"]["properties"]["from"]["x-expands-to-brushes"]
      == Json{true});

    const auto& fix = opNamed("fix");
    CHECK(fix["name"] == Json{"fix"});
    CHECK(
      fix["schema"]["required"] == parseJsonLiteral(R"(["op", "issue_type", "fix"])"));
    CHECK(
      fix["schema"]["properties"]["target"]["x-node-kinds"]
      == parseJsonLiteral(
        R"(["world", "layer", "group", "point_entity", "brush_entity", "brush", "patch", "face"])"));

    const auto& createPointEntity =
      opNamed("create_point_entity")["schema"]["properties"];
    CHECK(createPointEntity["properties"] == parseJsonLiteral(R"({
      "type": "object",
      "additionalProperties": {"type": "string"},
      "description": "more properties, with string values; not classname or origin"
    })"));
    CHECK(createPointEntity["allow_undefined"] == parseJsonLiteral(R"({
        "type": "boolean",
        "description": "create the entity even if the FGD does not define the classname as a point class",
        "default": false
      })"));

    const auto& createBrushEntity =
      opNamed("create_brush_entity")["schema"]["properties"];
    CHECK(
      createBrushEntity["brushes"]["x-node-kinds"] == parseJsonLiteral(R"(["brush"])"));
    CHECK(createBrushEntity["brushes"]["x-expands-to-brushes"] == Json{true});

    const auto& setSpawnflag = opNamed("set_spawnflag");
    CHECK(setSpawnflag["schema"]["properties"]["flag"]["oneOf"] == parseJsonLiteral(R"([
        {"type": "string", "minLength": 1},
        {"type": "integer", "minimum": 1, "maximum": 1073741824}
      ])"));
    CHECK(
      setSpawnflag["schema"]["properties"]["target"]["x-node-kinds"]
      == parseJsonLiteral(R"(["world", "point_entity", "brush_entity"])"));
  }

  SECTION("lists the issue types")
  {
    const auto json = runJson({"ops", "--json"});
    const auto& issueTypes = json["issue_types"];
    REQUIRE(issueTypes.size() == cli::issueTypes().size());

    const auto entry = [&](const std::string_view type) {
      for (const auto& issueType : issueTypes.asArray())
      {
        if (issueType["type"].asString() == type)
        {
          return issueType;
        }
      }
      FAIL("no issue type " << type);
      return Json{};
    };
    CHECK(
      entry("load_error")
      == parseJsonLiteral(R"({"type": "load_error", "severity": "error", "fixes": []})"));
    CHECK(
      entry("empty_group")
      == parseJsonLiteral(
        R"({"type": "empty_group", "severity": "warning", "fixes": ["delete_objects"]})"));
    CHECK(entry("long_property_value") == parseJsonLiteral(R"({
        "type": "long_property_value",
        "severity": "warning",
        "fixes": ["delete_property", "truncate_property_value"]
      })"));
    CHECK(
      entry("no_player_start")
      == parseJsonLiteral(
        R"({"type": "no_player_start", "severity": "error", "fixes": []})"));
    CHECK(
      entry("compiler")
      == parseJsonLiteral(R"({"type": "compiler", "severity": null, "fixes": []})"));
    CHECK(
      entry("compile_fatal")
      == parseJsonLiteral(
        R"({"type": "compile_fatal", "severity": "error", "fixes": []})"));
  }

  SECTION("JSON is the default")
  {
    CHECK(runJson({"ops"}) == runJson({"ops", "--json"}));
  }

  SECTION("the help lists every op")
  {
    const auto result = runCli({"--help"});
    CHECK_THAT(result.out, ContainsSubstring("  create_box\n"));
    CHECK_THAT(result.out, ContainsSubstring("  delete\n"));
    CHECK_THAT(result.out, ContainsSubstring("  fix\n"));
  }
}


TEST_CASE("Ops")
{
  SECTION("parseBatch")
  {
    SECTION("fills in defaults")
    {
      const auto ops = parseBatch(parseJsonLiteral(fmt::format("[{}]", CreateBoxOp)));
      REQUIRE(ops.size() == 1);
      CHECK(ops[0].def->name == "create_box");
      CHECK(ops[0].args["parent"] == Json{"layer:0"});
      CHECK(ops[0].binding() == std::nullopt);
    }

    SECTION("refuses a name bound twice")
    {
      const auto batch = parseJsonLiteral(R"([
        {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$b"},
        {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$b"}
      ])");
      try
      {
        parseBatch(batch);
        FAIL("parseBatch accepted a name bound twice");
      }
      catch (const CliError& e)
      {
        CHECK(e.code() == ErrorCode::InvalidArgument);
        CHECK(e.opIndex() == 1);
      }
    }

    SECTION("refuses a binding that no earlier op binds")
    {
      const auto batch = parseJsonLiteral(R"([
        {"op": "delete", "target": "$b"},
        {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$b"}
      ])");
      try
      {
        parseBatch(batch);
        FAIL("parseBatch accepted an unbound binding");
      }
      catch (const CliError& e)
      {
        CHECK(e.code() == ErrorCode::InvalidArgument);
        CHECK(e.opIndex() == 0);
      }
    }

    SECTION("accepts a binding that an earlier op binds")
    {
      const auto ops = parseBatch(parseJsonLiteral(R"([
        {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$b"},
        {"op": "delete", "target": ["layer:0/brush:0", "$b"]}
      ])"));
      REQUIRE(ops.size() == 2);
      CHECK(ops[1].bindingReferences() == std::vector<std::string>{"$b"});
    }

    SECTION("runs an op's own argument check")
    {
      const auto batch = parseJsonLiteral(R"([
        {"op": "delete", "target": "layer:0/brush:0"},
        {"op": "fix", "issue_type": "no_such_type", "fix": "delete_objects"}
      ])");
      try
      {
        parseBatch(batch);
        FAIL("parseBatch accepted an unknown issue type");
      }
      catch (const CliError& e)
      {
        CHECK(e.code() == ErrorCode::InvalidArgument);
        CHECK(e.opIndex() == 1);
      }
    }
  }

  SECTION("parseOpFlags")
  {
    const auto op = parseOpFlags(
      *findOpDef("create_box"),
      {{"min", "0,0,0"}, {"max", "8 8 8"}, {"material", "a"}, {"as", "$b"}});
    CHECK(op.args["min"] == parseJsonLiteral("[0, 0, 0]"));
    CHECK(op.args["max"] == parseJsonLiteral("[8, 8, 8]"));
    CHECK(op.binding() == "$b");

    SECTION("--query and --expect give the target")
    {
      CHECK(
        parseOpFlags(*findOpDef("delete"), {{"query", "true"}, {"expect", "2"}}).args
        == parseJsonLiteral(R"({"target": {"query": "true", "expect": 2}})"));
      CHECK(
        parseOpFlags(*findOpDef("delete"), {{"query", "true"}}).args
        == parseJsonLiteral(R"({"target": {"query": "true"}})"));
    }

    SECTION("bool, properties and spawnflag flags")
    {
      CHECK(
        parseOpFlags(
          *findOpDef("create_point_entity"),
          {{"classname", "light"},
           {"origin", "0,0,0"},
           {"properties", R"({"light": "200"})"},
           {"allow-undefined", "true"}})
          .args
        == parseJsonLiteral(R"({
          "classname": "light", "origin": [0, 0, 0], "properties": {"light": "200"},
          "allow_undefined": true, "parent": "layer:0"
        })"));

      const auto& setSpawnflag = *findOpDef("set_spawnflag");
      CHECK(
        parseOpFlags(
          setSpawnflag, {{"target", "world"}, {"flag", "256"}, {"on", "false"}})
          .args
        == parseJsonLiteral(R"({"target": "world", "flag": 256, "on": false})"));
      CHECK(
        parseOpFlags(
          setSpawnflag, {{"target", "world"}, {"flag", "Ambush"}, {"on", "true"}})
          .args["flag"]
        == Json{"Ambush"});
      CHECK_THROWS_AS(
        parseOpFlags(setSpawnflag, {{"target", "world"}, {"flag", "1"}, {"on", "yes"}}),
        CliError);
    }

    SECTION("points and shape kind flags")
    {
      const auto& createBrush = *findOpDef("create_brush");
      CHECK(
        parseOpFlags(
          createBrush, {{"vertices", "0,0,0, 8,0,0, 0,8,0, 0,0,8.5"}, {"material", "a"}})
          .args["vertices"]
        == parseJsonLiteral("[[0, 0, 0], [8, 0, 0], [0, 8, 0], [0, 0, 8.5]]"));
      // not a multiple of three numbers, and fewer than four points
      CHECK_THROWS_AS(
        parseOpFlags(
          createBrush, {{"vertices", "0,0,0,8,0,0,0,8,0,0,0"}, {"material", "a"}}),
        CliError);
      CHECK_THROWS_AS(
        parseOpFlags(createBrush, {{"vertices", "0,0,0,8,0,0,0,8,0"}, {"material", "a"}}),
        CliError);

      const auto& createShape = *findOpDef("create_shape");
      const auto shapeFlags = [](const std::string& kind) {
        return OpFlags{
          {"kind", kind},
          {"bbox", "0,0,0,64,64,64"},
          {"sides", "8"},
          {"axis", "z"},
          {"material", "a"}};
      };
      CHECK(parseOpFlags(createShape, shapeFlags("cone")).args["kind"] == Json{"cone"});
      CHECK_THROWS_AS(parseOpFlags(createShape, shapeFlags("sphere")), CliError);
    }
  }

  SECTION("isBoolFlag")
  {
    CHECK(isBoolFlag(*findOpDef("create_point_entity"), "allow-undefined"));
    CHECK(!isBoolFlag(*findOpDef("create_point_entity"), "allow_undefined"));
    CHECK(!isBoolFlag(*findOpDef("create_point_entity"), "classname"));
    CHECK(isBoolFlag(*findOpDef("set_spawnflag"), "on"));
  }

  SECTION("definesFlag")
  {
    CHECK(definesFlag(*findOpDef("set_uv"), "offset"));
    CHECK(definesFlag(*findOpDef("create_point_entity"), "allow-undefined"));
    CHECK(!definesFlag(*findOpDef("create_point_entity"), "allow_undefined"));
    CHECK(!definesFlag(*findOpDef("set_uv"), "query"));
    CHECK(!definesFlag(*findOpDef("set_uv"), "dry-run"));
  }

  SECTION("findOpDef")
  {
    CHECK(findOpDef("delete") != nullptr);
    CHECK(findOpDef("frobnicate") == nullptr);
  }
}

} // namespace tb::cli
