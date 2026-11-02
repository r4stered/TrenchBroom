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
#include "cli/Ops.h"

#include <fmt/format.h>

#include <optional>
#include <string>
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
    REQUIRE(json["ops"].size() == 2);
    CHECK(json["ops"][0]["name"] == Json{"create_box"});
    CHECK(json["ops"][1] == parseJsonLiteral(R"json({
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
  }

  SECTION("findOpDef")
  {
    CHECK(findOpDef("delete") != nullptr);
    CHECK(findOpDef("frobnicate") == nullptr);
  }
}

} // namespace tb::cli
