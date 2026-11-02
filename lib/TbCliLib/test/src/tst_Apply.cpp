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
#include "cli/LoadMap.h"

#include <fmt/format.h>

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

TEST_CASE("apply")
{
  auto fixture = EditFixture{};
  const auto& map = fixture.map;

  SECTION("runs a batch file and writes the map")
  {
    const auto batch =
      arg(fixture.env.writeFile("batch.json", fmt::format("[{}]", CreateBoxOp)));

    const auto json = runJson({"apply", map, batch});
    CHECK(json["schema"] == Json{"tbcli.apply/1"});
    CHECK(json["rev"] == Json{revOf(map)});
    CHECK(json["rev"] != Json{computeRev(fixture.original)});
    CHECK(json["ops"] == parseJsonLiteral(R"([{
      "ok": true,
      "created": ["layer:0/brush:2"],
      "modified": [],
      "removed": [],
      "warnings": []
    }])"));
    CHECK(json["bindings"] == Json::object());
    CHECK(json.find("warnings") == nullptr);
    CHECK(brushCount(map) == 4);
  }

  SECTION("reads the batch from stdin")
  {
    const auto result = fixture.apply(fmt::format("[{}]", CreateBoxOp));
    REQUIRE(result.exitCode == 0);
    CHECK(parseOutput(result)["rev"] == Json{revOf(map)});
    CHECK(brushCount(map) == 4);
  }

  SECTION("an empty batch rewrites the map unchanged in content")
  {
    const auto json = parseOutput(fixture.apply("[]"));
    CHECK(json["ops"] == Json::array());
    CHECK(json["rev"] == Json{revOf(map)});
    CHECK(brushCount(map) == 3);
  }

  SECTION("bindings name created nodes by their paths in the new rev")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$first"},
      {"op": "delete", "target": "layer:0/brush:0"},
      {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "b", "as": "$second"}
    ])"));

    // brush:0 is gone, so the boxes end up as brush:1 and brush:2
    CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));
    CHECK(json["ops"][2]["created"] == parseJsonLiteral(R"(["layer:0/brush:2"])"));
    CHECK(json["bindings"] == parseJsonLiteral(R"({
      "$first": "layer:0/brush:1",
      "$second": "layer:0/brush:2"
    })"));
    CHECK(
      runJson({"show", map, "layer:0/brush:2"})["node"]["textures"]
      == parseJsonLiteral(R"(["b"])"));
  }

  SECTION("a binding whose node is removed later in the batch is left out")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "create_box", "min": [16, 16, 0], "max": [32, 32, 8], "material": "a",
       "parent": "layer:0/entity:1", "as": "$box"},
      {"op": "delete", "target": "layer:0/entity:1"}
    ])"));
    CHECK(json["ops"][0]["created"] == Json::array());
    CHECK(json["bindings"] == Json::object());
  }

  SECTION("paths are resolved against the input rev before any op runs")
  {
    const auto json = parseOutput(fixture.apply(R"([
      {"op": "delete", "target": "layer:0/brush:0"},
      {"op": "delete", "target": "layer:0/brush:1"}
    ])"));
    CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));
    CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
    CHECK(brushCount(map) == 1);
  }

  SECTION("a failing op writes nothing and names its index")
  {
    const auto result = fixture.apply(
      fmt::format(
        R"([{}, {{"op": "delete", "target": "layer:0/brush:7"}}])", CreateBoxOp));
    checkError(result, "node_not_found", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:7"});
    CHECK(fixture.unchanged());

    checkError(
      fixture.apply(
        fmt::format(
          R"([{}, {{"op": "create_box", "min": [0, 0, 0], "max": [0, 8, 8], "material": "a"}}])",
          CreateBoxOp)),
      "invalid_argument",
      2,
      1);
    CHECK(fixture.unchanged());
  }

  SECTION("targeting a node removed earlier in the batch fails")
  {
    const auto result = fixture.apply(R"([
      {"op": "delete", "target": "layer:0/entity:1"},
      {"op": "delete", "target": "layer:0/entity:1/brush:0"}
    ])");
    checkError(result, "target_deleted", 1, 1);
    CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/entity:1/brush:0"});
    CHECK(fixture.unchanged());
  }

  SECTION("--expect-rev")
  {
    const auto rev = revOf(map);

    SECTION("a stale rev is refused with exit 3")
    {
      checkError(
        fixture.apply(fmt::format("[{}]", CreateBoxOp), {"--expect-rev", "000000000000"}),
        "stale_rev",
        3);
      CHECK(fixture.unchanged());
    }

    SECTION("the current rev is accepted")
    {
      const auto result =
        fixture.apply(fmt::format("[{}]", CreateBoxOp), {"--expect-rev", rev});
      CHECK(result.exitCode == 0);
      CHECK(brushCount(map) == 4);
    }
  }

  SECTION("--dry-run returns the full result and the would-be rev without writing")
  {
    const auto batch = fmt::format("[{}]", CreateBoxOp);
    const auto dryRun = parseOutput(fixture.apply(batch, {"--dry-run"}));
    CHECK(fixture.unchanged());

    const auto written = parseOutput(fixture.apply(batch));
    CHECK(dryRun == written);
  }

  SECTION("-o writes elsewhere and leaves the map as it is")
  {
    const auto out = arg(fixture.env.path("out/edited.map"));
    std::filesystem::create_directories(fixture.env.path("out"));

    const auto json =
      parseOutput(fixture.apply(fmt::format("[{}]", CreateBoxOp), {"-o", out}));
    CHECK(fixture.unchanged());
    CHECK(json["rev"] == Json{revOf(out)});
    CHECK(brushCount(out) == 4);
  }

  SECTION("an unconfirmed write fails with write_failed and leaves the original intact")
  {
    const auto target = GENERATE(
      std::string{"missing-dir/edited.map"},
      // renaming a file over a directory fails
      std::string{"blocked"});
    CAPTURE(target);
    std::filesystem::create_directories(fixture.env.path("blocked"));

    const auto out = arg(fixture.env.path(target));
    checkError(
      fixture.apply(fmt::format("[{}]", CreateBoxOp), {"-o", out}), "write_failed", 1);
    CHECK(fixture.unchanged());

    // the temporary file is cleaned up
    auto files = std::vector<std::string>{};
    for (const auto& entry : std::filesystem::directory_iterator{fixture.env.path("")})
    {
      files.push_back(entry.path().filename().string());
    }
    CHECK_THAT(
      files,
      UnorderedEquals(std::vector<std::string>{"blocked", "simple.map", "tbcli.json"}));
  }

  SECTION("malformed batches")
  {
    SECTION("invalid JSON is refused with its line")
    {
      const auto result = fixture.apply("[\n{\"op\": }]");
      checkError(result, "invalid_batch", 2);
      CHECK(parseOutput(result)["error"]["line"] == Json{2});
    }

    SECTION("a batch must be an array of objects")
    {
      checkError(fixture.apply(R"({"op": "delete"})"), "invalid_batch", 2);
      checkError(fixture.apply(R"([42])"), "invalid_batch", 2, 0);
    }

    SECTION("ops are checked against their definitions")
    {
      const auto op = GENERATE(
        R"({"target": "layer:0/brush:0"})",
        R"({"op": "frobnicate"})",
        R"({"op": "delete"})",
        R"({"op": "delete", "target": "layer:0/brush:0", "force": true})",
        R"({"op": "delete", "target": 3})",
        R"({"op": "create_box", "min": [0, 0], "max": [8, 8, 8], "material": "a"})",
        R"({"op": "create_box", "min": [0, 0, "0"], "max": [8, 8, 8], "material": "a"})",
        R"({"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "box"})",
        R"({"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": ""})");
      CAPTURE(op);
      checkError(
        fixture.apply(fmt::format("[{}, {}]", CreateBoxOp, op)),
        "invalid_argument",
        2,
        1);
      CHECK(fixture.unchanged());
    }

    SECTION("a malformed batch is not followed by the usage text")
    {
      const auto result = fixture.apply("[42]");
      CHECK(result.err.find("usage: tbcli") == std::string::npos);
    }

    SECTION("a missing batch file is refused")
    {
      checkError(
        runCli({"apply", map, arg(fixture.env.path("none.json"))}), "file_not_found", 1);
    }

    SECTION("apply requires a batch")
    {
      checkError(runCli({"apply", map}), "usage", 2);
    }
  }

  SECTION("mutating options are refused by other commands")
  {
    checkError(runCli({"describe", map, "--dry-run"}), "usage", 2);
    checkError(runCli({"show", map, "world", "--expect-rev", "x"}), "usage", 2);
    checkError(runCli({"query", map, "-o", "out.map"}), "usage", 2);
  }

  SECTION("targets")
  {
    // maps/targets.map: layer:0 holds brush:0, the brush entity entity:0 (func_door) and
    // group:0 (id 5) with two brushes; layer:1 is locked and holds brush:0 and the point
    // entity entity:0 (light); layer:2 is hidden and holds brush:0
    auto targetsFixture = EditFixture{"maps/targets.map"};
    const auto& targetsMap = targetsFixture.map;

    SECTION("bindings")
    {
      SECTION("later ops can target the nodes an earlier op bound")
      {
        const auto json = parseOutput(targetsFixture.apply(R"([
          {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$box"},
          {"op": "delete", "target": "$box"}
        ])"));
        CHECK(json["ops"][0]["created"] == Json::array());
        // the box was never in the input rev, so it has no path to report
        CHECK(json["ops"][1]["removed"] == Json::array());
        CHECK(json["bindings"] == Json::object());
        CHECK(brushCount(targetsMap) == 6);
      }

      SECTION("a list can mix paths and bindings")
      {
        const auto json = parseOutput(targetsFixture.apply(R"([
          {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$box"},
          {"op": "delete", "target": ["$box", "layer:0/brush:0"]}
        ])"));
        CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/brush:0"])"));
        CHECK(brushCount(targetsMap) == 5);
      }

      SECTION("a binding can name the parent of a later op")
      {
        // a binding to a brush is accepted as a path argument, and its kind is checked
        const auto result = targetsFixture.apply(R"([
          {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$box"},
          {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "parent": "$box"}
        ])");
        checkError(result, "wrong_node_kind", 1, 1);
        CHECK(parseOutput(result)["error"]["node"] == Json{"$box"});
      }

      SECTION("a binding must be bound by an earlier op")
      {
        checkError(
          targetsFixture.apply(R"([{"op": "delete", "target": "$box"}])"),
          "invalid_argument",
          2,
          0);
        checkError(
          targetsFixture.apply(R"([
            {"op": "delete", "target": ["layer:0/brush:0", "$box"]},
            {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$box"}
          ])"),
          "invalid_argument",
          2,
          0);
        checkError(
          runCli({"delete", targetsMap, "--target", "$box"}), "invalid_argument", 2);
        CHECK(targetsFixture.unchanged());
      }

      SECTION("created nodes are reachable only through bindings")
      {
        // layer:0/brush:1 is where the box ends up, but paths address the input rev
        const auto result = targetsFixture.apply(
          fmt::format(
            R"([{}, {{"op": "delete", "target": "layer:0/brush:1"}}])", CreateBoxOp));
        checkError(result, "node_not_found", 1, 1);
        CHECK(targetsFixture.unchanged());
      }

      SECTION("targeting a binding whose node was deleted fails")
      {
        const auto result = targetsFixture.apply(R"([
          {"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8], "material": "a", "as": "$box"},
          {"op": "delete", "target": "$box"},
          {"op": "delete", "target": "$box"}
        ])");
        checkError(result, "target_deleted", 1, 2);
        CHECK(parseOutput(result)["error"]["node"] == Json{"$box"});
        CHECK(targetsFixture.unchanged());
      }
    }

    SECTION("lists of paths")
    {
      SECTION("an op acts on every node of a list")
      {
        const auto json = parseOutput(targetsFixture.apply(R"([
          {"op": "delete", "target": ["layer:0/entity:0", "layer:0/brush:0"]}
        ])"));
        CHECK(
          json["ops"][0]["removed"]
          == parseJsonLiteral(R"(["layer:0/entity:0", "layer:0/brush:0"])"));
        CHECK(brushCount(targetsMap) == 4);
      }

      SECTION("a node named twice, or inside another named node, is acted on once")
      {
        const auto json =
          parseOutput(targetsFixture.apply(R"([{"op": "delete", "target": [
          "layer:0/group:0", "layer:0/group:0/brush:1", "layer:0/group:0"
        ]}])"));
        CHECK(
          json["ops"][0]["removed"]
          == parseJsonLiteral(R"(["layer:0/group:0", "layer:0/group:0/brush:1"])"));
        CHECK(brushCount(targetsMap) == 4);
      }

      SECTION("a node deleted earlier in the batch fails")
      {
        const auto result = targetsFixture.apply(R"([
          {"op": "delete", "target": "layer:0/group:0"},
          {"op": "delete", "target": ["layer:0/brush:0", "layer:0/group:0/brush:0"]}
        ])");
        checkError(result, "target_deleted", 1, 1);
        CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/group:0/brush:0"});
        CHECK(targetsFixture.unchanged());
      }

      SECTION("every path is resolved up front")
      {
        const auto result = targetsFixture.apply(R"([
          {"op": "delete", "target": ["layer:0/brush:0", "layer:0/brush:9"]}
        ])");
        checkError(result, "node_not_found", 1, 0);
        CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:9"});
      }
    }

    SECTION("query targets")
    {
      SECTION("act on every match")
      {
        const auto json = parseOutput(targetsFixture.apply(R"([
          {"op": "delete", "target": {"query": "classname is \"func_door\"", "expect": 1}}
        ])"));
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));
        CHECK(brushCount(targetsMap) == 5);
      }

      SECTION("expect defaults to any")
      {
        const auto json = parseOutput(targetsFixture.apply(R"([
          {"op": "delete", "target": {"query": "classname is \"nothing\""}},
          {"op": "delete", "target": {"query": "classname is \"func_door\"", "expect": "any"}}
        ])"));
        CHECK(json["ops"][0]["removed"] == Json::array());
        CHECK(json["ops"][1]["removed"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));
      }

      SECTION("a match count other than expect fails with expect_mismatch")
      {
        const auto result = targetsFixture.apply(
          fmt::format(
            R"([{}, {{"op": "delete", "target": {{"query": "type is \"brush\"", "expect": 4}}}}])",
            CreateBoxOp));
        checkError(result, "expect_mismatch", 1, 1);
        CHECK_THAT(
          parseOutput(result)["error"]["message"].asString(), ContainsSubstring("5"));
        CHECK(targetsFixture.unchanged());
      }

      SECTION("resolve against the input rev")
      {
        // the box the first op creates is not matched, and the door brush that the second
        // op deletes is still matched, so the third op fails
        const auto result = targetsFixture.apply(
          fmt::format(
            R"([{}, {{"op": "delete", "target": "layer:0/entity:0"}},
              {{"op": "delete", "target": {{"query": "type is \"brush\"", "expect": 5}}}}])",
            CreateBoxOp));
        checkError(result, "target_deleted", 1, 2);
        CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/entity:0/brush:0"});
      }

      SECTION("a match of a kind the op does not accept fails")
      {
        const auto result = targetsFixture.apply(R"([
          {"op": "delete", "target": {"query": "material is \"floor\""}}
        ])");
        checkError(result, "wrong_node_kind", 1, 0);
        CHECK(parseOutput(result)["error"]["node"] == Json{"layer:0/brush:0/face:0"});
      }

      SECTION("malformed targets are refused")
      {
        const auto target = GENERATE(
          R"({"query": 3})",
          R"({"expect": 1})",
          R"({"query": "true", "expect": -1})",
          R"({"query": "true", "expect": 1.5})",
          R"({"query": "true", "expect": "some"})",
          R"({"query": "true", "limit": 2})",
          R"([])",
          R"([3])",
          R"("$1box")");
        CAPTURE(target);
        checkError(
          targetsFixture.apply(
            fmt::format(R"([{{"op": "delete", "target": {}}}])", target)),
          "invalid_argument",
          2,
          0);
      }
    }

    SECTION("locks")
    {
      SECTION("an op on a locked node fails")
      {
        const auto target = GENERATE("layer:1/brush:0", "layer:1/entity:0");
        CAPTURE(target);
        const auto result = runCli({"delete", targetsMap, "--target", target});
        checkError(result, "locked", 1, 0);
        CHECK(parseOutput(result)["error"]["node"] == Json{target});
        CHECK(targetsFixture.unchanged());
      }

      SECTION("an op in a locked container fails")
      {
        checkError(
          targetsFixture.apply(
            R"([{"op": "create_box", "min": [0, 0, 0], "max": [8, 8, 8],
                             "material": "a", "parent": "layer:1"}])"),
          "locked",
          1,
          0);
        CHECK(targetsFixture.unchanged());
      }

      SECTION("--allow-locked lets ops edit locked nodes")
      {
        const auto json = runJson(
          {"delete", targetsMap, "--target", "layer:1/brush:0", "--allow-locked"});
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:1/brush:0"])"));
        CHECK(brushCount(targetsMap) == 5);
      }

      SECTION("locked nodes are dropped from query results before the expect check")
      {
        checkError(
          targetsFixture.apply(R"([{"op": "delete", "target":
            {"query": "classname is \"light\"", "expect": 1}}])"),
          "expect_mismatch",
          1,
          0);

        const auto json = parseOutput(targetsFixture.apply(R"([{"op": "delete", "target":
          {"query": "type is \"brush\"", "expect": 5}}])"));
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"([
          "layer:0/brush:0",
          "layer:0/entity:0/brush:0",
          "layer:0/group:0/brush:0",
          "layer:0/group:0/brush:1",
          "layer:2/brush:0",
          "layer:0/entity:0",
          "layer:0/group:0"
        ])"));
      }

      SECTION("--allow-locked keeps locked nodes in query results")
      {
        const auto json = parseOutput(targetsFixture.apply(
          R"([{"op": "delete", "target": {"query": "classname is \"light\"", "expect": 1}}])",
          {"--allow-locked"}));
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:1/entity:0"])"));
      }

      SECTION("hidden nodes are targeted like any other")
      {
        const auto json = runJson({"delete", targetsMap, "--target", "layer:2/brush:0"});
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:2/brush:0"])"));
      }

      SECTION("--allow-locked is refused by other commands")
      {
        checkError(runCli({"describe", targetsMap, "--allow-locked"}), "usage", 2);
      }
    }

    SECTION("subcommand targets")
    {
      SECTION("--query and --expect give a query target")
      {
        const auto json = runJson(
          {"delete",
           targetsMap,
           "--query",
           R"(classname is "func_door")",
           "--expect",
           "1"});
        CHECK(json["ops"][0]["removed"] == parseJsonLiteral(R"(["layer:0/entity:0"])"));
      }

      SECTION("--expect accepts any and defaults to it")
      {
        CHECK(
          runJson({"delete", targetsMap, "--query", R"(type is "brush")", "--dry-run"})
          == runJson(
            {"delete",
             targetsMap,
             "--query",
             R"(type is "brush")",
             "--expect",
             "any",
             "--dry-run"}));
      }

      SECTION("a match count other than --expect fails")
      {
        checkError(
          runCli(
            {"delete", targetsMap, "--query", R"(type is "brush")", "--expect", "2"}),
          "expect_mismatch",
          1,
          0);
        CHECK(targetsFixture.unchanged());
      }

      SECTION("malformed target flags are refused")
      {
        const auto flags = GENERATE(
          std::vector<std::string>{"--expect", "1"},
          std::vector<std::string>{"--target", "layer:0/brush:0", "--query", "true"},
          std::vector<std::string>{"--query", "true", "--expect", "-1"},
          std::vector<std::string>{"--query", "true", "--expect", "x"},
          std::vector<std::string>{"--query", "true", "--query", "true"});
        auto args = std::vector<std::string>{"delete", targetsMap};
        args.insert(args.end(), flags.begin(), flags.end());
        CAPTURE(args);
        checkError(runCli(args), "invalid_argument", 2);
        CHECK(targetsFixture.unchanged());
      }

      SECTION("ops without a target refuse --query")
      {
        checkError(
          runCli(
            {"create_box",
             targetsMap,
             "--min=0,0,0",
             "--max=8,8,8",
             "--material=a",
             "--query=true"}),
          "invalid_argument",
          2);
      }
    }
  }

  SECTION("after a lossy load")
  {
    // maps/lossy.map is maps/simple.map with a face whose plane points coincide
    auto lossyFixture = EditFixture{"maps/lossy.map"};
    const auto& lossyMap = lossyFixture.map;

    SECTION("mutating commands refuse to write")
    {
      const auto result = lossyFixture.apply(fmt::format("[{}]", CreateBoxOp));
      checkError(result, "lossy_load", 1);
      CHECK_THAT(
        parseOutput(result)["error"]["message"].asString(),
        ContainsSubstring("--allow-lossy"));
      CHECK(lossyFixture.unchanged());

      checkError(
        runCli({"delete", lossyMap, "--target", "layer:0/brush:0"}), "lossy_load", 1);
      CHECK(lossyFixture.unchanged());
    }

    SECTION("a dry run previews the edit with a lossy_load warning")
    {
      const auto json =
        parseOutput(lossyFixture.apply(fmt::format("[{}]", CreateBoxOp), {"--dry-run"}));
      CHECK(json["ops"][0]["created"] == parseJsonLiteral(R"(["layer:0/brush:1"])"));
      CHECK(json["warnings"] == parseJsonLiteral(R"(["lossy_load"])"));
      CHECK(lossyFixture.unchanged());
    }

    SECTION("--allow-lossy writes anyway")
    {
      const auto result =
        lossyFixture.apply(fmt::format("[{}]", CreateBoxOp), {"--allow-lossy"});
      REQUIRE(result.exitCode == 0);
      CHECK(parseOutput(result)["warnings"] == parseJsonLiteral(R"(["lossy_load"])"));
      CHECK(!lossyFixture.unchanged());
    }

    SECTION("read commands still work")
    {
      CHECK(runCli({"describe", lossyMap}).exitCode == 0);
    }

    SECTION("--allow-lossy is refused by other commands")
    {
      checkError(runCli({"describe", lossyMap, "--allow-lossy"}), "usage", 2);
    }
  }
}

} // namespace tb::cli
