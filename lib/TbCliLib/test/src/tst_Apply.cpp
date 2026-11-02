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
}

} // namespace tb::cli
