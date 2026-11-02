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
#include "cli/IssueTypes.h"
#include "cli/NodePath.h"
#include "cli/Verify.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

/*
 * maps/verify.map has, in file order:
 *   world: empty_property_value (wad)
 *   layer:0/brush:1: non_integer_vertices
 *   layer:0/entity:1 (light): link_target, empty_property_value (message, noise)
 *   layer:0/entity:2 (func_door): link_source, and invalid_uv_scale on brush:0/face:0
 *   layer:0/entity:3 (no classname): missing_classname, missing_definition
 *   layer:0/group:0: empty_group
 */
constexpr auto VerifyMapIssueCount = size_t{10};

/** A verify fixture: maps/verify.map with a tbcli.json that has the given verify key. */
struct VerifyFixture
{
  CliTestEnvironment env;
  std::string map;

  explicit VerifyFixture(
    const std::filesystem::path& fixture = "maps/verify.map",
    const std::optional<Json>& verifyConfig = std::nullopt)
  {
    auto config = quakeConfig();
    if (verifyConfig)
    {
      config.set("verify", *verifyConfig);
    }
    env.writeConfig({}, config);
    map = arg(env.copyFixture(fixture, fixture.filename()));
  }

  RunResult run(const std::vector<std::string>& options = {}) const
  {
    auto args = std::vector<std::string>{"verify", map};
    args.insert(args.end(), options.begin(), options.end());
    return runCli(args);
  }

  Json json(const std::vector<std::string>& options = {}) const
  {
    const auto result = run(options);
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    return parseOutput(result);
  }
};

std::vector<std::string> field(const Json& items, const std::string_view name)
{
  auto result = std::vector<std::string>{};
  for (const auto& item : items.asArray())
  {
    result.push_back(item[name].isNull() ? "null" : item[name].asString());
  }
  return result;
}

/** The keys of the listed issues by node path and type. */
std::map<std::string, std::string> keysByNodeAndType(const Json& json)
{
  auto result = std::map<std::string, std::string>{};
  for (const auto& issue : json["issues"]["items"].asArray())
  {
    const auto& node = issue["node"];
    result
      [(node.isNull() ? "null" : node.asString()) + " " + issue["type"].asString() + " "
       + issue["message"].asString()] = issue["key"].asString();
  }
  return result;
}

/*
 * A verify --compile fixture: a map from fixture/compiled copied into a temp dir with a
 * tbcli.json whose compilers.qbsp is the fake compiler replaying the map's recording.
 *
 * compile_warnings.map: qbsp warns about the mixed water and wall brush layer:0/brush:9
 * by line (92, one of its faces) and about the info_null layer:0/entity:2 by
 * classname and origin; six more warnings name no node.
 *
 * compile_fatal.map: qbsp warns that there is no wad and fails on the region brush
 * layer:0/entity:2/brush:0 (line 105), exiting with 1.
 */
struct CompileFixture
{
  CliTestEnvironment env;
  std::string map;
  std::filesystem::path qbsp;

  /**
   * The fake qbsp sleeps for `sleepMs`; `timeout` is its timeout in tbcli.json, in
   * seconds.
   */
  explicit CompileFixture(
    const std::string& recording = "compile_warnings",
    const int sleepMs = 0,
    const std::optional<double> timeout = std::nullopt,
    const std::optional<Json>& verifyConfig = std::nullopt)
  {
    qbsp = setUpFakeQbsp(env, recording, sleepMs);
    auto tool = Json::object();
    tool.set("path", arg(qbsp));
    if (timeout)
    {
      tool.set("timeout", *timeout);
    }
    auto compilers = Json::object();
    compilers.set("qbsp", std::move(tool));
    auto config = quakeConfig();
    config.set("compilers", std::move(compilers));
    if (verifyConfig)
    {
      config.set("verify", *verifyConfig);
    }
    env.writeConfig({}, config);
    const auto fixture = std::filesystem::path{"compiled"} / (recording + ".map");
    map = arg(env.copyFixture(fixture, fixture.filename()));
  }

  RunResult run(const std::vector<std::string>& options = {"--compile"}) const
  {
    auto args = std::vector<std::string>{"verify", map};
    args.insert(args.end(), options.begin(), options.end());
    return runCli(args);
  }

  Json json(const std::vector<std::string>& options = {"--compile"}) const
  {
    const auto result = run(options);
    INFO(result.out);
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    return parseOutput(result);
  }
};

/** The listed issues of the given type. */
std::vector<Json> issuesOfType(const Json& json, const std::string_view type)
{
  auto result = std::vector<Json>{};
  for (const auto& issue : json["issues"]["items"].asArray())
  {
    if (issue["type"].asString() == type)
    {
      result.push_back(issue);
    }
  }
  return result;
}

const Json& stageNamed(const Json& json, const std::string_view name)
{
  const auto& stages = json["stages"].asArray();
  const auto it = std::ranges::find_if(
    stages, [&](const auto& stage) { return stage["name"].asString() == name; });
  REQUIRE(it != stages.end());
  return *it;
}

} // namespace

TEST_CASE("verify")
{
  SECTION("reports every validator issue in file order")
  {
    const auto json = VerifyFixture{}.json();
    CHECK(json["schema"] == Json{"tbcli.verify/1"});
    CHECK(json["rev"].isString());

    const auto& items = json["issues"]["items"];
    CHECK(
      field(items, "type")
      == std::vector<std::string>{
        "empty_property_value",
        "non_integer_vertices",
        "link_target",
        "empty_property_value",
        "empty_property_value",
        "link_source",
        "invalid_uv_scale",
        "missing_classname",
        "missing_definition",
        "empty_group",
      });
    CHECK(
      field(items, "node")
      == std::vector<std::string>{
        "world",
        "layer:0/brush:1",
        "layer:0/entity:1",
        "layer:0/entity:1",
        "layer:0/entity:1",
        "layer:0/entity:2",
        "layer:0/entity:2/brush:0/face:0",
        "layer:0/entity:3",
        "layer:0/entity:3",
        "layer:0/group:0",
      });
    CHECK(json["issues"]["total"] == Json{VerifyMapIssueCount});
    CHECK(json["issues"]["truncated"] == Json{false});
  }

  SECTION("an issue record")
  {
    const auto json = VerifyFixture{}.json({"--type", "missing_classname"});
    REQUIRE(json["issues"]["items"].size() == 1);
    auto issue = json["issues"]["items"][0];
    const auto key = issue["key"].asString();
    CHECK(key.size() == 12);
    CHECK(key.find_first_not_of("0123456789abcdef") == std::string::npos);

    issue.set("key", "<key>");
    CHECK(issue == parseJsonLiteral(R"({
      "type": "missing_classname",
      "severity": "error",
      "key": "<key>",
      "message": "Entity has no classname property",
      "node": "layer:0/entity:3",
      "line": 55,
      "origin": [32, 32, 32],
      "fixes": ["delete_objects"]
    })"));
  }

  SECTION("brush issues carry bounds")
  {
    const auto json =
      VerifyFixture{}.json({"--type", "non_integer_vertices,invalid_uv_scale"});
    const auto& items = json["issues"]["items"];
    REQUIRE(items.size() == 2);
    CHECK(items[0]["bounds"] == parseJsonLiteral(R"({
      "min": [-64, -64, 0], "max": [-48.5, -48.5, 128]
    })"));
    CHECK(items[0].find("origin") == nullptr);
    // a face issue has the bounds of the face, and the line the map reader gives the
    // face, which is the end of the line before it
    CHECK(items[1]["bounds"] == parseJsonLiteral(R"({
      "min": [16, -16, 0], "max": [16, 16, 96]
    })"));
    CHECK(items[1]["line"] == Json{45});
  }

  SECTION("issues of one type on one node have different keys")
  {
    const auto json = VerifyFixture{}.json({"--type", "empty_property_value"});
    const auto& items = json["issues"]["items"];
    REQUIRE(items.size() == 3);
    CHECK(items[1]["node"] == items[2]["node"]);
    CHECK(items[1]["key"] != items[2]["key"]);
  }

  SECTION("each item the reader skipped is a load_error with its line")
  {
    const auto fixture = VerifyFixture{"maps/lossy.map"};
    const auto json = fixture.json({"--type", "load_error"});

    // the reader skips the face, and then reports its brush as incomplete; each has
    // its own line, so the brush comes first
    const auto& items = json["issues"]["items"];
    REQUIRE(items.size() == 2);
    CHECK_THAT(items[0]["message"].asString(), StartsWith("Brush is incomplete"));
    CHECK(items[0]["line"] == Json{18});
    CHECK_THAT(items[1]["message"].asString(), StartsWith("Skipping face"));
    CHECK(items[1]["line"] == Json{19});
    for (const auto& issue : items.asArray())
    {
      CHECK(issue["severity"] == Json{"error"});
      CHECK(issue["node"] == Json{nullptr});
      CHECK(issue.find("origin") == nullptr);
      CHECK(issue.find("bounds") == nullptr);
    }
    CHECK(items[0]["key"] != items[1]["key"]);
    CHECK(json["counts"]["load_error"] == Json{2});
    CHECK(json["ok"] == Json{false});
  }

  SECTION("CLI checks")
  {
    const auto json = VerifyFixture{"maps/cli_checks.map"}.json(
      {"--type",
       "fgd_property_type,entity_inside_solid,no_player_start,degenerate_brush"});

    // see tst_CliValidators.cpp for what maps/cli_checks.map holds
    const auto& items = json["issues"]["items"];
    CHECK(
      field(items, "type")
      == std::vector<std::string>{
        "no_player_start",
        "fgd_property_type",
        "fgd_property_type",
        "degenerate_brush",
        "degenerate_brush",
        "fgd_property_type",
        "entity_inside_solid",
        "entity_inside_solid",
      });
    CHECK(
      field(items, "node")
      == std::vector<std::string>{
        "null",
        "world",
        "world",
        "layer:0/brush:2",
        "layer:0/brush:3",
        "layer:0/entity:0",
        "layer:0/entity:0",
        "layer:0/entity:5",
      });
    CHECK(json["counts"]["no_player_start"] == Json{1});
    CHECK(json["counts"]["entity_inside_solid"] == Json{2});
    CHECK(json["ok"] == Json{false});

    SECTION("a missing player start is about the whole map")
    {
      auto issue = items[0];
      CHECK(issue["key"].asString().size() == 12);
      CHECK_THAT(issue["message"].asString(), StartsWith("Map has no player start"));
      issue.set("key", "<key>");
      issue.set("message", "<message>");
      CHECK(issue == parseJsonLiteral(R"({
        "type": "no_player_start",
        "severity": "error",
        "key": "<key>",
        "message": "<message>",
        "node": null,
        "line": null,
        "fixes": []
      })"));
    }

    SECTION("an entity inside a solid has its origin")
    {
      CHECK(items[6]["origin"] == parseJsonLiteral("[0, 0, -8]"));
      CHECK(items[6]["severity"] == Json{"error"});
    }
  }

  SECTION("missing_material is reported only with a Quake directory")
  {
    auto fixture = VerifyFixture{"maps/cli_checks.map"};
    const auto missingMaterial = std::vector<std::string>{"--type", "missing_material"};
    const auto unchecked = fixture.json(missingMaterial);
    CHECK(unchecked["issues"]["total"] == Json{0});
    CHECK(
      unchecked["stages"][2]["unchecked"] == parseJsonLiteral(R"(["missing_material"])"));

    fixture.env.copyFile(texturesFixturePath() / "cr8_a_excerpt.wad", "textures.wad");
    fixture.env.copyFile(
      texturesFixturePath() / "palette.lmp", "quake/id1/gfx/palette.lmp");
    auto config = quakeConfig();
    config.set("quake_dir", arg(fixture.env.path("quake")));
    fixture.env.writeConfig({}, config);

    // only the water pool's material is not in the wad
    const auto json = fixture.json(missingMaterial);
    CHECK(json["stages"][2].find("unchecked") == nullptr);
    CHECK(json["issues"]["total"] == Json{6});
    CHECK(json["issues"]["items"][0]["node"] == Json{"layer:0/brush:1/face:0"});
    CHECK(json["issues"]["items"][0]["message"] == Json{"Material '*water1' not found"});
    CHECK(json["issues"]["items"][0]["severity"] == Json{"warning"});
  }

  SECTION("stages")
  {
    const auto stagesOf = [](const Json& json) {
      auto result = std::vector<std::string>{};
      for (const auto& stage : json["stages"].asArray())
      {
        CHECK(stage["ms"].isNumber());
        result.push_back(stage["name"].asString() + " " + stage["status"].asString());
      }
      return result;
    };

    CHECK(
      stagesOf(VerifyFixture{}.json())
      == std::vector<std::string>{
        "load_diagnostics ok", "validators issues", "cli_checks ok"});
    CHECK(
      stagesOf(VerifyFixture{"maps/lossy.map"}.json())
      == std::vector<std::string>{
        "load_diagnostics issues", "validators issues", "cli_checks ok"});
    CHECK(
      stagesOf(VerifyFixture{"maps/cli_checks.map"}.json())
      == std::vector<std::string>{
        "load_diagnostics ok", "validators issues", "cli_checks issues"});

    // a stage whose only issues are suppressed is ok
    const auto quiet = VerifyFixture{
      "maps/simple.map",
      parseJsonLiteral(
        R"({"ignore": [{"type": "empty_property_value"}, {"type": "link_source"}]})")};
    CHECK(
      stagesOf(quiet.json())
      == std::vector<std::string>{
        "load_diagnostics ok", "validators ok", "cli_checks ok"});
  }

  SECTION("counts cover the whole map")
  {
    const auto expectedCounts = parseJsonLiteral(R"({
      "missing_classname": 1,
      "missing_definition": 1,
      "empty_group": 1,
      "link_source": 1,
      "link_target": 1,
      "non_integer_vertices": 1,
      "empty_property_value": 3,
      "invalid_uv_scale": 1,
      "suppressed": 0
    })");
    const auto expectedBySeverity =
      parseJsonLiteral(R"({"error": 1, "warning": 8, "info": 1})");

    const auto options = GENERATE(
      std::vector<std::string>{},
      std::vector<std::string>{"--limit", "1", "--offset", "2"},
      std::vector<std::string>{"--type", "empty_group"},
      std::vector<std::string>{"--min-severity", "error"},
      std::vector<std::string>{"--target", "layer:0/entity:1"});
    CAPTURE(options);

    const auto json = VerifyFixture{}.json(options);
    CHECK(json["counts"] == expectedCounts);
    CHECK(json["by_severity"] == expectedBySeverity);
  }

  SECTION("paging")
  {
    const auto fixture = VerifyFixture{};
    const auto all = fixture.json();

    const auto page = fixture.json({"--limit", "2", "--offset", "3"});
    REQUIRE(page["issues"]["items"].size() == 2);
    CHECK(page["issues"]["items"][0] == all["issues"]["items"][3]);
    CHECK(page["issues"]["items"][1] == all["issues"]["items"][4]);
    CHECK(page["issues"]["total"] == Json{VerifyMapIssueCount});
    CHECK(page["issues"]["truncated"] == Json{true});

    const auto last = fixture.json({"--offset", "8"});
    CHECK(last["issues"]["items"].size() == 2);
    CHECK(last["issues"]["truncated"] == Json{false});

    const auto beyond = fixture.json({"--offset", "20"});
    CHECK(beyond["issues"]["items"].size() == 0);
    CHECK(beyond["issues"]["total"] == Json{VerifyMapIssueCount});
    CHECK(beyond["issues"]["truncated"] == Json{false});
  }

  SECTION("filters")
  {
    const auto fixture = VerifyFixture{};
    const auto types = [&](const std::vector<std::string>& options) {
      return field(fixture.json(options)["issues"]["items"], "type");
    };

    SECTION("by type")
    {
      CHECK(
        types({"--type", "link_target,link_source"})
        == std::vector<std::string>{"link_target", "link_source"});
      CHECK(
        types({"--type", "link_target", "--type", "empty_group"})
        == std::vector<std::string>{"link_target", "empty_group"});
    }

    SECTION("by minimum severity")
    {
      CHECK(
        types({"--min-severity", "error"})
        == std::vector<std::string>{"missing_classname"});
      CHECK(types({"--min-severity", "warning"}).size() == VerifyMapIssueCount - 1);
      CHECK(types({"--min-severity", "info"}).size() == VerifyMapIssueCount);
    }

    SECTION("by a target path, including what is inside it")
    {
      CHECK(
        types({"--target", "layer:0/entity:1"})
        == std::vector<std::string>{
          "link_target", "empty_property_value", "empty_property_value"});
      CHECK(
        types({"--target", "layer:0/entity:2"})
        == std::vector<std::string>{"link_source", "invalid_uv_scale"});
      CHECK(
        types({"--target", "layer:0/entity:2/brush:0/face:0"})
        == std::vector<std::string>{"invalid_uv_scale"});
      CHECK(types({"--target", "world"}).size() == VerifyMapIssueCount);
    }

    SECTION("by a target query")
    {
      CHECK(
        types({"--target", R"(classname is "func_door")"})
        == std::vector<std::string>{"link_source", "invalid_uv_scale"});
    }

    SECTION("combined")
    {
      CHECK(
        types({"--target", "layer:0/entity:1", "--type", "link_target"})
        == std::vector<std::string>{"link_target"});
    }

    SECTION("an unknown type is a usage error")
    {
      checkError(fixture.run({"--type", "no_such_type"}), "usage", 2);
    }

    SECTION("an unknown severity is a usage error")
    {
      checkError(fixture.run({"--min-severity", "fatal"}), "usage", 2);
      checkError(fixture.run({"--fail-on", "fatal"}), "usage", 2);
    }

    SECTION("a target path that does not resolve fails")
    {
      checkError(fixture.run({"--target", "layer:0/entity:99"}), "node_not_found", 1);
    }
  }

  SECTION("keys are stable across revs while the node is unchanged")
  {
    auto fixture = VerifyFixture{};
    const auto before = keysByNodeAndType(fixture.json());
    REQUIRE(before.size() == VerifyMapIssueCount);

    // a new worldspawn property shifts every later line and changes the world's text
    auto contents = fixture.env.readFile("verify.map");
    const auto worldspawn = std::string{"\"classname\" \"worldspawn\"\n"};
    contents.replace(
      contents.find(worldspawn), worldspawn.size(), worldspawn + "\"_tb_x\" \"1\"\n");
    // and a moved light changes the light's text
    const auto origin = std::string{"\"origin\" \"0 32 64\""};
    contents.replace(contents.find(origin), origin.size(), "\"origin\" \"0 32 80\"");
    fixture.env.writeFile("verify.map", contents);

    const auto after = keysByNodeAndType(fixture.json());
    REQUIRE(after.size() == before.size());
    for (const auto& [issue, key] : before)
    {
      CAPTURE(issue);
      const auto changed =
        issue.starts_with("world ") || issue.starts_with("layer:0/entity:1 ");
      CHECK((after.at(issue) != key) == changed);
    }
  }

  SECTION("keys are the same in every run")
  {
    // groups without a link id in the file get a random one on every load
    const auto fixture = VerifyFixture{};
    CHECK(keysByNodeAndType(fixture.json()) == keysByNodeAndType(fixture.json()));
  }

  SECTION("tbcli.json")
  {
    SECTION("verify.severity overrides the severity of a type")
    {
      const auto fixture = VerifyFixture{
        "maps/verify.map",
        parseJsonLiteral(
          R"({"severity": {"missing_classname": "info", "empty_group": "error"}})")};
      const auto json = fixture.json({"--type", "missing_classname,empty_group"});
      CHECK(
        field(json["issues"]["items"], "severity")
        == std::vector<std::string>{"info", "error"});
      CHECK(
        json["by_severity"]
        == parseJsonLiteral(R"({"error": 1, "warning": 7, "info": 2})"));
    }

    SECTION("verify.ignore suppresses a type")
    {
      const auto fixture = VerifyFixture{
        "maps/verify.map",
        parseJsonLiteral(R"({"ignore": [{"type": "empty_property_value"}]})")};
      const auto json = fixture.json();
      CHECK(json["issues"]["total"] == Json{VerifyMapIssueCount - 3});
      CHECK(json["counts"].find("empty_property_value") == nullptr);
      CHECK(json["counts"]["suppressed"] == Json{3});
      CHECK(json["by_severity"]["warning"] == Json{5});
    }

    SECTION("verify.ignore suppresses one issue by key")
    {
      const auto key =
        VerifyFixture{}.json({"--target", "world"})["issues"]["items"][0]["key"];
      auto ignore = Json::object();
      ignore.set("type", "empty_property_value");
      ignore.set("key", key);
      auto config = Json::object();
      config.set("ignore", Json{Json::Array{ignore}});

      const auto json = VerifyFixture{"maps/verify.map", config}.json();
      CHECK(json["counts"]["empty_property_value"] == Json{2});
      CHECK(json["counts"]["suppressed"] == Json{1});
      CHECK(json["issues"]["items"][0]["node"] == Json{"layer:0/brush:1"});
    }

    SECTION("verify.ignore suppresses issues on and inside what a query matches")
    {
      const auto fixture =
        VerifyFixture{"maps/verify.map", parseJsonLiteral(R"({"ignore": [
          {"type": "empty_property_value", "query": "classname is \"light\""},
          {"type": "invalid_uv_scale", "query": "classname is \"func_door\""}
        ]})")};
      const auto json = fixture.json();
      CHECK(json["counts"]["empty_property_value"] == Json{1});
      CHECK(json["counts"].find("invalid_uv_scale") == nullptr);
      CHECK(json["counts"]["suppressed"] == Json{3});
    }

    SECTION("suppressed issues do not fail verify")
    {
      const auto fixture = VerifyFixture{
        "maps/verify.map",
        parseJsonLiteral(R"({"ignore": [{"type": "missing_classname"}]})")};
      const auto result = fixture.run({"--fail-on", "error"});
      CHECK(result.exitCode == 0);
      CHECK(parseOutput(result)["ok"] == Json{true});
    }
  }

  SECTION("ok and --fail-on")
  {
    const auto fixture = VerifyFixture{};

    SECTION("ok is false with an error, and the exit code is 0 without --fail-on")
    {
      const auto result = fixture.run();
      CHECK(result.exitCode == 0);
      CHECK(parseOutput(result)["ok"] == Json{false});
    }

    SECTION("--fail-on exits 4 when an issue is at or above it")
    {
      const auto result = fixture.run({"--fail-on", "error"});
      CHECK(result.exitCode == 4);
      const auto json = parseOutput(result);
      CHECK(json["ok"] == Json{false});
      CHECK(json["schema"] == Json{"tbcli.verify/1"});
    }

    SECTION("--fail-on sets the threshold of ok")
    {
      // maps/simple.map has only warnings: worldspawn's empty wad and an untargeted door
      const auto simple = VerifyFixture{"maps/simple.map"};
      CHECK(simple.json()["ok"] == Json{true});
      CHECK(simple.run({"--fail-on", "error"}).exitCode == 0);

      const auto result = simple.run({"--fail-on", "warning"});
      CHECK(result.exitCode == 4);
      CHECK(parseOutput(result)["ok"] == Json{false});
    }

    SECTION("filters do not change ok or the exit code")
    {
      const auto result = fixture.run({"--fail-on", "error", "--type", "empty_group"});
      CHECK(result.exitCode == 4);
      CHECK(parseOutput(result)["ok"] == Json{false});
    }
  }

  SECTION("--compile")
  {
    SECTION("turns the qbsp log into compiler issues mapped to nodes")
    {
      const auto json = CompileFixture{}.json();

      const auto compilerIssues = issuesOfType(json, "compiler");
      REQUIRE(compilerIssues.size() == 8);
      CHECK(json["counts"]["compiler"] == Json{8});
      for (const auto& issue : compilerIssues)
      {
        CHECK(issue["tool"] == Json{"qbsp"});
        CHECK(issue["severity"] == Json{"warning"});
        CHECK(issue["unreliable_mapping"].isNull());
      }

      // issues without a line come first, then in file order
      CHECK(
        compilerIssues[0]
        == parseJsonLiteral(
          R"({
          "type": "compiler",
          "severity": "warning",
          "key": ")"
          + compilerIssues[0]["key"].asString() + R"(",
          "message": "No wad or _wad key exists in the worldmodel",
          "node": null,
          "line": null,
          "tool": "qbsp",
          "fixes": []
        })"));

      // mapped by [line N] to the brush containing the line
      const auto& byLine = compilerIssues[6];
      CHECK_THAT(
        byLine["message"].asString(),
        StartsWith("compile_warnings.map[line 92]: brush has multiple face contents"));
      CHECK(byLine["node"] == Json{"layer:0/brush:9"});
      CHECK(byLine["line"] == Json{92});
      CHECK(byLine["bounds"].isObject());

      // mapped by classname and origin to the entity, with the entity's line
      const auto& byOrigin = compilerIssues[7];
      CHECK_THAT(byOrigin["message"].asString(), StartsWith("info_null at -192 64 32.5"));
      CHECK(byOrigin["node"] == Json{"layer:0/entity:2"});
      CHECK(byOrigin["line"] == Json{110});
      CHECK(byOrigin["origin"] == parseJsonLiteral("[-192, 64, 32.5]"));
    }

    SECTION("adds a qbsp stage after the in-process stages")
    {
      const auto json = CompileFixture{}.json();
      auto names = std::vector<std::string>{};
      for (const auto& stage : json["stages"].asArray())
      {
        names.push_back(stage["name"].asString());
      }
      CHECK(
        names
        == std::vector<std::string>{
          "load_diagnostics", "validators", "cli_checks", "qbsp"});
      CHECK(stageNamed(json, "qbsp")["status"] == Json{"issues"});
      CHECK(stageNamed(json, "qbsp")["ms"].isNumber());
    }

    SECTION("without --compile, qbsp does not run")
    {
      const auto fixture = CompileFixture{};
      const auto json = fixture.json({});
      CHECK(issuesOfType(json, "compiler").empty());
      CHECK(json["stages"].size() == 3);
      CHECK_FALSE(std::filesystem::exists(fixture.env.path("tools/qbsp.args")));
    }

    SECTION("--compile=bsp is --compile")
    {
      const auto json = CompileFixture{}.json({"--compile=bsp"});
      CHECK(stageNamed(json, "qbsp")["status"] == Json{"issues"});
    }

    SECTION("qbsp compiles the exact bytes, with -keepprt and without -leaktest")
    {
      const auto fixture = CompileFixture{};
      fixture.json();
      CHECK(
        fixture.env.readFile("tools/qbsp.args") == "-keepprt\ncompile_warnings.map\n");
      CHECK(
        fixture.env.readFile("tools/qbsp.input")
        == fixture.env.readFile("compile_warnings.map"));
    }

    SECTION("a failed qbsp run is a compile_fatal issue, not a failed command")
    {
      const auto json = CompileFixture{"compile_fatal"}.json();
      CHECK(stageNamed(json, "qbsp")["status"] == Json{"failed"});
      CHECK(json["ok"] == Json{false});

      const auto fatal = issuesOfType(json, "compile_fatal");
      REQUIRE(fatal.size() == 1);
      CHECK(fatal[0]["severity"] == Json{"error"});
      CHECK(fatal[0]["message"] == Json{"qbsp exited with code 1"});
      CHECK(fatal[0]["node"] == Json{});
      CHECK(fatal[0]["tool"] == Json{"qbsp"});

      // the error that made it fail is a compiler issue on the brush
      const auto compilerIssues = issuesOfType(json, "compiler");
      REQUIRE(compilerIssues.size() == 2);
      CHECK(compilerIssues[1]["severity"] == Json{"error"});
      CHECK(compilerIssues[1]["node"] == Json{"layer:0/entity:2/brush:0"});
      CHECK(compilerIssues[1]["line"] == Json{105});
    }

    SECTION("timeouts")
    {
      SECTION("qbsp is stopped after the timeout in tbcli.json")
      {
        const auto json = CompileFixture{"compile_warnings", 10000, 0.2}.json();
        CHECK(stageNamed(json, "qbsp")["status"] == Json{"failed"});
        const auto fatal = issuesOfType(json, "compile_fatal");
        REQUIRE(fatal.size() == 1);
        CHECK(
          fatal[0]["message"]
          == Json{"qbsp did not finish within 0.2 seconds and was stopped"});
      }

      SECTION("--timeout overrides tbcli.json")
      {
        const auto json = CompileFixture{"compile_warnings", 10000, 600}.json(
          {"--compile", "--timeout", "0.1"});
        const auto fatal = issuesOfType(json, "compile_fatal");
        REQUIRE(fatal.size() == 1);
        CHECK(
          fatal[0]["message"]
          == Json{"qbsp did not finish within 0.1 seconds and was stopped"});
      }

      SECTION("a malformed --timeout is a usage error")
      {
        const auto fixture = CompileFixture{};
        checkError(fixture.run({"--compile", "--timeout", "0"}), "usage", 2);
        checkError(fixture.run({"--compile", "--timeout", "soon"}), "usage", 2);
      }
    }

    SECTION("compiler paths")
    {
      auto fixture = CompileFixture{};
      const auto writeQbspPath = [&](const std::optional<std::filesystem::path>& path) {
        auto config = quakeConfig();
        if (path)
        {
          auto compilers = Json::object();
          compilers.set("qbsp", arg(*path));
          config.set("compilers", std::move(compilers));
        }
        fixture.env.writeConfig({}, config);
      };

      SECTION("a missing qbsp is compiler_missing")
      {
        writeQbspPath(std::nullopt);
        checkError(fixture.run(), "compiler_missing", 1);

        writeQbspPath(fixture.env.path("tools/missing"));
        checkError(fixture.run(), "compiler_missing", 1);
      }

      SECTION("--qbsp overrides tbcli.json")
      {
        writeQbspPath(fixture.env.path("tools/missing"));
        const auto json = fixture.json({"--compile", "--qbsp", arg(fixture.qbsp)});
        CHECK(stageNamed(json, "qbsp")["status"] == Json{"issues"});
      }
    }

    SECTION("artifacts")
    {
      const auto fixture = CompileFixture{};

      SECTION("are deleted without --artifacts")
      {
        fixture.json();
        const auto workDir =
          std::filesystem::path{fixture.env.readFile("tools/qbsp.cwd")};
        CHECK_FALSE(std::filesystem::exists(workDir));
        CHECK_FALSE(std::filesystem::exists(fixture.env.path("compile_warnings.bsp")));
      }

      SECTION("are kept with --artifacts, with a sidecar")
      {
        const auto json =
          fixture.json({"--compile", "--artifacts", arg(fixture.env.path("out"))});
        for (const auto* file :
             {"out/compile_warnings.bsp",
              "out/compile_warnings.prt",
              "out/compile_warnings.log"})
        {
          CAPTURE(file);
          CHECK(std::filesystem::is_regular_file(fixture.env.path(file)));
        }
        CHECK(
          parseJsonLiteral(fixture.env.readFile("out/compile_warnings.bsp.tbcli.json"))
          == parseJsonLiteral(
            R"({"rev": ")" + json["rev"].asString() + R"(", "leaked": false})"));
      }

      SECTION("--timeout and --artifacts require --compile")
      {
        checkError(fixture.run({"--timeout", "1"}), "usage", 2);
        checkError(fixture.run({"--artifacts", "out"}), "usage", 2);
      }
    }

    SECTION("--compile=vis and --compile=light are not supported yet")
    {
      const auto fixture = CompileFixture{};
      checkError(fixture.run({"--compile=vis"}), "usage", 2);
      checkError(fixture.run({"--compile=light"}), "usage", 2);
    }

    SECTION("other commands do not accept the compile options")
    {
      const auto fixture = CompileFixture{};
      checkError(runCli({"describe", fixture.map, "--compile"}), "usage", 2);
    }

    SECTION("after a lossy load, compiler issues have unreliable_mapping")
    {
      auto fixture = CompileFixture{};
      // a brush with a face that does not parse, after everything qbsp's log names
      fixture.env.writeFile(
        "compile_warnings.map",
        fixture.env.readFile("compile_warnings.map") + R"(// entity 4
  {
  "classname" "func_wall"
  {
  ( 0 0 0 ) ( 0 0 0 ) ( 0 0 0 ) wall1_1 [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1
  }
  }
  )");
      const auto json = fixture.json();
      CHECK(json["counts"]["load_error"].isNumber());
      const auto compilerIssues = issuesOfType(json, "compiler");
      REQUIRE(compilerIssues.size() == 8);
      for (const auto& issue : compilerIssues)
      {
        CHECK(issue["unreliable_mapping"] == Json{true});
      }
      CHECK(compilerIssues[6]["node"] == Json{"layer:0/brush:9"});
    }

    SECTION("compiler issue keys do not depend on the temporary directory")
    {
      const auto fixture = CompileFixture{};
      const auto keys = [&]() {
        auto result = std::vector<std::string>{};
        for (const auto& issue : issuesOfType(fixture.json(), "compiler"))
        {
          result.push_back(issue["key"].asString());
        }
        return result;
      };
      const auto first = keys();
      CHECK(first == keys());
      // and each issue has its own key
      CHECK(std::set<std::string>(first.begin(), first.end()).size() == first.size());
    }

    SECTION("verify.ignore suppresses compiler issues, and the stage is ok")
    {
      const auto json =
        CompileFixture{
          "compile_warnings",
          0,
          std::nullopt,
          parseJsonLiteral(R"({"ignore": [{"type": "compiler"}]})")}
          .json();
      CHECK(issuesOfType(json, "compiler").empty());
      CHECK(json["counts"]["suppressed"] == Json{8});
      CHECK(stageNamed(json, "qbsp")["status"] == Json{"ok"});
    }

    SECTION("--type and --target filter compiler issues")
    {
      const auto json = CompileFixture{}.json(
        {"--compile", "--type", "compiler", "--target", "layer:0/brush:9"});
      REQUIRE(json["issues"]["items"].size() == 1);
      CHECK(json["issues"]["items"][0]["line"] == Json{92});
    }
  }
}

TEST_CASE("NodeIssueCollector")
{
  SECTION("collect")
  {
    auto logger = NullLogger{};
    auto loadedMap = loadFixtureMap("maps/verify.map", logger);
    auto& worldNode = loadedMap.map->worldNode();
    auto collector = NodeIssueCollector{loadedMap, worldNode.registeredValidators()};

    // the light's issues, but not those of the nodes below the layer
    auto* light = resolveNodePath(worldNode, "layer:0/entity:1").node;
    auto issues = collector.collect(*light);
    REQUIRE(issues.size() == 3);
    CHECK(issues[0].node == light);
    CHECK(collector.collect(*worldNode.defaultLayer()).empty());

    // property issues carry their property
    auto properties = std::vector<std::string>{};
    for (const auto& issue : issues)
    {
      if (issue.type->id == "empty_property_value")
      {
        properties.push_back(issue.propertyKey.value_or(""));
      }
    }
    CHECK(properties == std::vector<std::string>{"message", "noise"});
  }
}

} // namespace tb::cli
