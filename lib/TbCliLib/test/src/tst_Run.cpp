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
#include "FakeEditorHost.h"
#include "cli/CliError.h"
#include "cli/Compile.h"
#include "cli/FileUtils.h"
#include "cli/Guide.h"
#include "cli/View.h"
#include "fs/TestUtils.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_NodeLocking.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Selection.h"
#include "mdl/TransactionScope.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/string_utils.h"

#include "vm/vec.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
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

void checkErrorEnvelope(
  const RunResult& result, const std::string& code, const int exitCode)
{
  CHECK(result.exitCode == exitCode);

  const auto json = parseOutput(result);
  REQUIRE(json.isObject());
  CHECK(json.size() == 1);

  const auto& error = json["error"];
  REQUIRE(error.isObject());
  CHECK(error["code"] == Json{code});
  CHECK(error["message"].isString());
  CHECK(!error["message"].asString().empty());
}

/** The counter of the live rev `live-<epoch>:<counter>` that the output reports. */
size_t liveRevCounter(const Json& output)
{
  const auto& rev = output["rev"].asString();
  return std::stoul(rev.substr(rev.find(':') + 1));
}

/** A node path in live output and the handle next to it. */
struct PathAndHandle
{
  std::string path;
  std::string handle;
};

/** A list of paths or handles, or one of them as a list of one. */
Json asList(const Json& json)
{
  return json.isArray() ? json : Json{Json::Array{json}};
}

/**
 * Collects the paths in live output that have a handle next to them: `path` and
 * `handle`, `<key>` and `<key>_handle`, and the lists `<key>` and `<key>_handles`, or
 * objects of paths or lists of them, as the apply result's `bindings`.
 */
void collectPathsAndHandles(const Json& json, std::vector<PathAndHandle>& result)
{
  const auto zip = [&](const Json& paths, const Json& handles) {
    REQUIRE(paths.size() == handles.size());
    for (size_t i = 0; i < paths.size(); ++i)
    {
      result.push_back({paths[i].asString(), handles[i].asString()});
    }
  };

  if (json.isArray())
  {
    for (const auto& element : json.asArray())
    {
      collectPathsAndHandles(element, result);
    }
  }
  else if (json.isObject())
  {
    for (const auto& [key, value] : json.asObject())
    {
      if (key == "path")
      {
        REQUIRE(json.find("handle") != nullptr);
        result.push_back({value.asString(), json["handle"].asString()});
      }
      else if (
        const auto* handle = json.find(key + "_handle"); handle && value.isString())
      {
        result.push_back({value.asString(), handle->asString()});
      }
      else if (const auto* handles = json.find(key + "_handles"); handles)
      {
        if (value.isObject())
        {
          for (const auto& [name, bound] : value.asObject())
          {
            zip(asList(bound), asList((*handles)[name]));
          }
        }
        else
        {
          zip(value, *handles);
        }
      }
      else
      {
        collectPathsAndHandles(value, result);
      }
    }
  }
}

std::vector<PathAndHandle> pathsAndHandles(const Json& json)
{
  auto result = std::vector<PathAndHandle>{};
  collectPathsAndHandles(json, result);
  return result;
}

double maxY(const RunResult& result)
{
  return parseOutput(result)["bounds"]["max"][1].asNumber();
}

} // namespace

TEST_CASE("run")
{
  auto env = CliTestEnvironment{};

  SECTION("command line")
  {
    SECTION("no arguments is a usage error")
    {
      const auto result = runCli({});
      checkErrorEnvelope(result, "usage", 2);
      CHECK_THAT(result.err, ContainsSubstring("usage: tbcli"));
    }

    SECTION("unknown command is a usage error")
    {
      checkErrorEnvelope(runCli({"frobnicate", "x.map"}), "usage", 2);
    }

    SECTION("unknown option is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--frobnicate"}), "usage", 2);
    }

    SECTION("option without its value is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--fgd"}), "usage", 2);
    }

    SECTION("unknown output format is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--format", "xml"}), "usage", 2);
    }

    SECTION("single dash option is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "-x"}), "usage", 2);
    }

    SECTION("describe without a map is a usage error")
    {
      checkErrorEnvelope(runCli({"describe"}), "usage", 2);
    }

    SECTION("describe with two maps is a usage error")
    {
      checkErrorEnvelope(runCli({"describe", "a.map", "b.map"}), "usage", 2);
    }

    SECTION("show without a node path is a usage error")
    {
      checkErrorEnvelope(runCli({"show", "x.map"}), "usage", 2);
    }

    SECTION("fgd without a classname is a usage error")
    {
      checkErrorEnvelope(runCli({"fgd", "x.map"}), "usage", 2);
    }

    SECTION("a malformed count is a usage error")
    {
      const auto count = GENERATE("many", "-1", "+1", "1.5", "");
      CAPTURE(count);
      checkErrorEnvelope(runCli({"query", "x.map", "--limit", count}), "usage", 2);
      checkErrorEnvelope(runCli({"query", "x.map", "--offset", count}), "usage", 2);
    }

    SECTION("a malformed box is a usage error")
    {
      const auto box = GENERATE(
        "1,2,3", "1,2,3,4,5,6,7", "0,0,0,1,1,x", "0,0,0,1,1,1e", "10,0,0,0,10,10");
      CAPTURE(box);
      checkErrorEnvelope(runCli({"query", "x.map", "--within", box}), "usage", 2);
      checkErrorEnvelope(runCli({"query", "x.map", "--intersects", box}), "usage", 2);
    }

    SECTION("options of other commands are refused")
    {
      checkErrorEnvelope(runCli({"query", "x.map", "--raw"}), "usage", 2);
      checkErrorEnvelope(runCli({"describe", "x.map", "--limit", "1"}), "usage", 2);
      checkErrorEnvelope(
        runCli({"show", "x.map", "world", "--key-entities", "light"}), "usage", 2);
      checkErrorEnvelope(runCli({"fgd", "x.map", "light", "--count-only"}), "usage", 2);
      checkErrorEnvelope(runCli({"describe", "x.map", "--fail-on", "error"}), "usage", 2);
      checkErrorEnvelope(
        runCli({"query", "x.map", "--min-severity", "error"}), "usage", 2);
    }

    SECTION("an option that several commands accept is accepted by each of them")
    {
      // accepted, so the command gets as far as loading the map
      checkErrorEnvelope(runCli({"query", "x.map", "--limit", "1"}), "file_not_found", 1);
      checkErrorEnvelope(
        runCli({"verify", "x.map", "--limit", "1"}), "file_not_found", 1);
    }

    SECTION("an op's --target is the op's flag, not verify's option")
    {
      checkErrorEnvelope(
        runCli({"delete", "x.map", "--target", "layer:0/brush:0"}), "file_not_found", 1);
    }

    SECTION("an op's argument wins over an option of the same name")
    {
      // set_uv's --offset, not the paging option that op commands refuse
      checkErrorEnvelope(
        runCli({"set_uv", "x.map", "--target", "layer:0/brush:0", "--offset", "8,0"}),
        "file_not_found",
        1);
      checkErrorEnvelope(
        runCli({"set_uv", "x.map", "--target", "layer:0/brush:0", "--offset", "x"}),
        "invalid_argument",
        2);
      checkErrorEnvelope(
        runCli({"query", "x.map", "--offset", "8"}), "file_not_found", 1);

      // the op's other flags and the mutating options still parse
      checkErrorEnvelope(
        runCli(
          {"set_uv",
           "x.map",
           "--target",
           "layer:0/brush:0",
           "--offset",
           "8,0",
           "--dry-run",
           "--rotation",
           "90"}),
        "file_not_found",
        1);
    }

    SECTION("an op's bool flag may leave out its value")
    {
      using Args = std::vector<std::string>;
      const auto flags = GENERATE(
        Args{"--flag", "1", "--on"},
        Args{"--on", "--flag", "1"},
        Args{"--flag", "1", "--on", "true"},
        Args{"--flag", "1", "--on", "false"},
        Args{"--flag", "1", "--on=false"});
      CAPTURE(flags);

      auto args = Args{"set_spawnflag", "x.map", "--target", "world"};
      args.insert(args.end(), flags.begin(), flags.end());
      // accepted, so the command gets as far as loading the map
      checkErrorEnvelope(runCli(args), "file_not_found", 1);
    }

    SECTION("an op's bool flag refuses other values")
    {
      checkErrorEnvelope(
        runCli(
          {"set_spawnflag", "x.map", "--target", "world", "--flag", "1", "--on=yes"}),
        "invalid_argument",
        2);
    }

    SECTION("--help and -h print usage")
    {
      const auto option = GENERATE("--help", "-h");
      const auto result = runCli({option});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, ContainsSubstring("usage: tbcli"));
    }

    SECTION("--version prints the version")
    {
      const auto result = runCli({"--version"});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, StartsWith("tbcli "));
    }
  }

  SECTION("map loading")
  {
    env.writeConfig({}, quakeConfig());

    SECTION("missing map fails")
    {
      checkErrorEnvelope(
        runCli({"describe", arg(env.path("missing.map"))}), "file_not_found", 1);
    }

    SECTION("logger output goes to stderr")
    {
      const auto map = env.copyFixture("maps/simple.map", "simple.map");
      const auto result = runCli({"describe", arg(map)});

      CHECK(result.exitCode == 0);
      CHECK_THAT(result.err, ContainsSubstring("Loading document from"));
      CHECK_THAT(result.err, ContainsSubstring("Loaded entity definition file"));
      CHECK(parseOutput(result).isObject());
    }
  }

  SECTION("exit codes")
  {
    SECTION("a command can exit with its own code and still print its result")
    {
      env.writeConfig({}, quakeConfig());
      const auto map = env.copyFixture("maps/simple.map", "simple.map");

      // maps/simple.map has warnings
      const auto result = runCli({"verify", arg(map), "--fail-on", "warning"});
      CHECK(result.exitCode == 4);
      CHECK(parseOutput(result)["schema"] == Json{"tbcli.verify/1"});
    }
  }

  SECTION("configuration")
  {
    const auto map = env.copyFixture("maps/simple.map", "project/maps/sub/simple.map");

    SECTION("tbcli.json is found by walking up from the map")
    {
      env.writeConfig("project", quakeConfig());

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      // the Quake FGD makes the player start 16 units wide on each side
      CHECK(maxY(result) == 96.0);
    }

    SECTION("without a game config the load fails")
    {
      checkErrorEnvelope(runCli({"describe", arg(map)}), "config_error", 1);
    }

    SECTION("relative paths are resolved against the directory of tbcli.json")
    {
      env.copyFixture("fgd/big_start.fgd", "project/defs/big_start.fgd");
      auto config = quakeConfig();
      config.set("fgd", "defs/big_start.fgd");
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
    }

    SECTION("only the first of several FGDs is loaded")
    {
      auto config = quakeConfig();
      config.set(
        "fgd",
        Json{Json::Array{arg(fixturePath("fgd/big_start.fgd")), arg(quakeFgdPath())}});
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
      CHECK_THAT(result.err, ContainsSubstring("Only the first FGD is loaded"));
    }

    SECTION("a missing FGD fails")
    {
      auto config = quakeConfig();
      config.set("fgd", "missing.fgd");
      env.writeConfig("project", config);

      checkErrorEnvelope(runCli({"describe", arg(map)}), "config_error", 1);
    }

    SECTION("a malformed tbcli.json fails with its line")
    {
      env.writeFile("project/tbcli.json", "{\n  \"format\": \"valve\",\n  oops\n}");

      const auto result = runCli({"describe", arg(map)});
      checkErrorEnvelope(result, "config_error", 1);
      CHECK(parseOutput(result)["error"]["line"] == Json{3});
    }

    SECTION("a non-Valve format is refused")
    {
      SECTION("in tbcli.json")
      {
        auto config = quakeConfig();
        config.set("format", "standard");
        env.writeConfig("project", config);

        checkErrorEnvelope(runCli({"describe", arg(map)}), "not_valve_220", 1);
      }

      SECTION("as a flag")
      {
        env.writeConfig("project", quakeConfig());

        checkErrorEnvelope(
          runCli({"describe", arg(map), "--map-format", "standard"}), "not_valve_220", 1);
      }
    }

    SECTION("a missing Quake directory is reported and ignored")
    {
      auto config = quakeConfig();
      config.set("quake_dir", "missing");
      env.writeConfig("project", config);

      const auto result = runCli({"describe", arg(map)});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.err, ContainsSubstring("Quake directory"));
      CHECK_THAT(result.err, ContainsSubstring("not found"));
    }

    SECTION("flags override tbcli.json")
    {
      auto config = quakeConfig();
      config.set("format", "standard");
      config.set("game_config", "missing.cfg");
      config.set("fgd", "missing.fgd");
      config.set("quake_dir", "missing");
      env.writeConfig("project", config);
      std::filesystem::create_directories(env.path("quake"));

      const auto result = runCli(
        {"describe",
         arg(map),
         "--map-format",
         "valve",
         "--game-config",
         arg(quakeGameConfigPath()),
         "--fgd=" + arg(fixturePath("fgd/big_start.fgd")),
         "--quake-dir",
         arg(env.path("quake"))});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 112.0);
    }

    SECTION("--key-entities overrides describe.key_entities")
    {
      auto describe = Json::object();
      describe.set("key_entities", Json{Json::Array{"func_door"}});
      auto config = quakeConfig();
      config.set("describe", describe);
      env.writeConfig("project", config);

      // empty elements are ignored
      const auto result = runCli({"describe", arg(map), "--key-entities", ",light,"});
      CHECK(result.exitCode == 0);

      const auto output = parseOutput(result);
      const auto& items = output["key_entities"]["items"];
      CHECK(items.size() == 3);
      CHECK(items[1]["classname"] == Json{"light"});
      // listed because it has a targetname, not because of its class
      CHECK(items[2]["classname"] == Json{"func_door"});
    }

    SECTION("flags work without tbcli.json")
    {
      const auto result = runCli(
        {"describe",
         arg(map),
         "--game-config",
         arg(quakeGameConfigPath()),
         "--fgd",
         arg(quakeFgdPath())});
      CHECK(result.exitCode == 0);
      CHECK(maxY(result) == 96.0);
    }
  }

  SECTION("guide")
  {
    SECTION("prints the guide as Markdown")
    {
      const auto result = runCli({"guide"});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, StartsWith("# tbcli guide\n"));
      CHECK(result.out == guideText(std::nullopt));
    }

    SECTION("prints a topic")
    {
      const auto result = runCli({"guide", "targeting"});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, StartsWith("# Targeting\n"));
    }

    SECTION("an unknown topic is a usage error")
    {
      checkErrorEnvelope(runCli({"guide", "frobnicate"}), "usage", 2);
    }

    SECTION("--help names the topics that read tbcli.json")
    {
      CHECK_THAT(runCli({"--help"}).out, ContainsSubstring("guide conventions [<map>]"));
    }

    SECTION("only conventions takes a map")
    {
      checkErrorEnvelope(runCli({"guide", "verify", "x.map"}), "usage", 2);
      checkErrorEnvelope(runCli({"guide", "conventions", "x.map", "y.map"}), "usage", 2);
    }

    SECTION("conventions come from the tbcli.json found from the map")
    {
      env.writeFile(
        "project/tbcli.json",
        R"({"guide": {"conventions": {"Player hull": "8 x 8 x 8"}}})");

      const auto result =
        runCli({"guide", "conventions", arg(env.path("project/maps/x.map"))});
      CHECK(result.exitCode == 0);
      CHECK_THAT(result.out, ContainsSubstring("| Player hull | 8 x 8 x 8 |"));
      CHECK_THAT(result.out, !ContainsSubstring("Step height"));
    }

    SECTION("a malformed tbcli.json fails only the conventions topic")
    {
      env.writeFile("project/tbcli.json", R"({"guide": []})");
      const auto map = arg(env.path("project/x.map"));
      checkErrorEnvelope(runCli({"guide", "conventions", map}), "config_error", 1);
    }

    SECTION("--install writes the agent files")
    {
      const auto dir = env.path("skills/tbcli");
      const auto result = runCli({"guide", "--install", arg(dir)});
      CHECK(result.exitCode == 0);

      const auto output = parseOutput(result);
      CHECK(output["schema"] == Json{"tbcli.guide_install/1"});
      CHECK(output["skill"] == Json{arg((dir / "SKILL.md").lexically_normal())});
      CHECK_THAT(env.readFile("skills/tbcli/SKILL.md"), StartsWith("---\n"));
    }

    SECTION("--install takes no topic")
    {
      checkErrorEnvelope(
        runCli({"guide", "verify", "--install", arg(env.path("skills"))}), "usage", 2);
    }

    SECTION("other commands refuse --install")
    {
      checkErrorEnvelope(runCli({"describe", "x.map", "--install", "dir"}), "usage", 2);
    }
  }
}

TEST_CASE("run with a live session")
{
  auto fixture = EditFixture{};
  auto editor = FakeEditorHost{fixture.env.path(fixture.fileName)};

  const auto liveJson =
    [&](const std::vector<std::string>& args, const std::string_view in = {}) {
      const auto result = editor.run(args, in);
      INFO(result.out);
      INFO(result.err);
      REQUIRE(result.exitCode == 0);
      return parseOutput(result);
    };
  const auto liveBrushCount = [&]() {
    return liveJson({"describe", fixture.map})["counts"]["brushes"].asNumber();
  };

  SECTION("describe")
  {
    const auto json = liveJson({"describe", fixture.map});
    CHECK(json["rev"] == Json{editor.rev()});
    CHECK_THAT(json["rev"].asString(), StartsWith("live-" + editor.epoch() + ":"));
    CHECK(
      json["session"]
      == Json{
        Json::Object{{"live", true}, {"epoch", editor.epoch()}, {"map", fixture.map}}});

    // apart from the rev, the session block and the handles, as in file mode
    CHECK(
      withoutHandles(withoutKeys(json, {"rev", "session"}))
      == withoutKeys(runJson({"describe", fixture.map}), {"rev"}));
  }

  SECTION("reads report the live rev")
  {
    CHECK(liveJson({"query", fixture.map})["rev"] == Json{editor.rev()});
    CHECK(liveJson({"show", fixture.map, "world"})["rev"] == Json{editor.rev()});
  }

  SECTION("the live rev goes up on every change and ignores selection")
  {
    auto& map = editor.map();
    const auto rev0 = liveRevCounter(liveJson({"describe", fixture.map}));

    liveJson({"apply", fixture.map, "-"}, "[" + std::string{CreateBoxOp} + "]");
    const auto rev1 = liveRevCounter(liveJson({"describe", fixture.map}));
    CHECK(rev1 > rev0);

    map.undoCommand();
    const auto rev2 = liveRevCounter(liveJson({"describe", fixture.map}));
    CHECK(rev2 > rev1);

    map.redoCommand();
    const auto rev3 = liveRevCounter(liveJson({"describe", fixture.map}));
    CHECK(rev3 > rev2);

    mdl::selectAllNodes(map);
    mdl::deselectAll(map);
    CHECK(liveRevCounter(liveJson({"describe", fixture.map})) == rev3);
  }

  SECTION("apply")
  {
    auto& map = editor.map();
    const auto batch = std::string{R"([
      {"op": "create_box", "min": [0, 0, 0], "max": [64, 32, 16], "material": "floor",
       "as": "$box"},
      {"op": "set_property", "target": "layer:0/entity:2", "key": "light",
       "value": "300"},
      {"op": "translate", "target": "$box", "delta": [0, 0, 8]}
    ])"};

    SECTION("edits the open map as one undo step and writes nothing")
    {
      const auto brushesBefore = liveBrushCount();
      const auto json = liveJson({"apply", fixture.map, "-"}, batch);

      CHECK(json["session"] == Json{"live"});
      CHECK(json["rev"] == Json{editor.rev()});
      CHECK(liveBrushCount() == brushesBefore + 1);
      CHECK(fixture.unchanged());

      REQUIRE(map.undoCommandName() != nullptr);
      CHECK(*map.undoCommandName() == "AI: create_box, set_property +1 more");
      map.undoCommand();
      CHECK(liveBrushCount() == brushesBefore);
      CHECK(!map.canUndoCommand());

      // apart from the rev, the session and the handles, as in file mode
      CHECK(
        withoutHandles(withoutKeys(json, {"rev", "session"}))
        == withoutKeys(parseOutput(fixture.apply(batch)), {"rev"}));
    }

    SECTION("an op runs the same as a one-op batch")
    {
      const auto json = liveJson(
        {"set_property",
         fixture.map,
         "--target",
         "layer:0/entity:2",
         "--key",
         "light",
         "--value",
         "300"});
      CHECK(json["session"] == Json{"live"});
      CHECK(*map.undoCommandName() == "AI: set_property");
      CHECK(fixture.unchanged());
    }

    SECTION("a failing op leaves the map unchanged with the file-mode error")
    {
      const auto failing = std::string{R"([
        {"op": "create_box", "min": [0, 0, 0], "max": [64, 32, 16], "material": "floor",
         "as": "$box"},
        {"op": "set_property", "target": "$box", "key": "light", "value": "300"}
      ])"};
      const auto brushesBefore = liveBrushCount();

      // the second op fails after the first one has edited the map
      const auto result = editor.run({"apply", fixture.map, "-"}, failing);
      CHECK(result.out == fixture.apply(failing).out);
      checkError(result, "wrong_node_kind", 1, 1);

      CHECK(liveBrushCount() == brushesBefore);
      CHECK(!map.canUndoCommand());
      CHECK(map.isCommandCollationEnabled());
    }

    SECTION("keeps the mapper's node selection, apart from removed nodes")
    {
      auto* layer = map.worldNode().defaultLayer();
      const auto& children = layer->children();
      auto* playerStart = children[2];
      auto* light = children[4];
      mdl::selectNodes(map, {playerStart, light});

      liveJson(
        {"apply", fixture.map, "-"},
        R"([{"op": "delete", "target": "layer:0/entity:0"}, )" + std::string{CreateBoxOp}
          + "]");

      CHECK(map.selection().nodes == std::vector<mdl::Node*>{light});
      map.undoCommand();
      CHECK(map.selection().nodes == std::vector<mdl::Node*>{playerStart, light});
    }

    SECTION("keeps the mapper's face selection")
    {
      auto* brush =
        dynamic_cast<mdl::BrushNode*>(map.worldNode().defaultLayer()->children()[0]);
      REQUIRE(brush != nullptr);
      const auto face = mdl::BrushFaceHandle{*brush, 2};
      mdl::selectBrushFaces(map, {face});

      liveJson({"apply", fixture.map, "-"}, batch);

      CHECK(map.selection().brushFaces == std::vector<mdl::BrushFaceHandle>{face});
    }

    SECTION("--dry-run returns the result and leaves the map as it is")
    {
      const auto brushesBefore = liveBrushCount();
      const auto json = liveJson({"apply", fixture.map, "-", "--dry-run"}, batch);
      // undoing the batch is a change too
      CHECK(json["rev"] == Json{editor.rev()});
      CHECK(liveBrushCount() == brushesBefore);
      CHECK(
        withoutHandles(json["ops"])
        == parseOutput(fixture.apply(batch, {"--dry-run"}))["ops"]);
      CHECK(!map.canUndoCommand());
    }

    SECTION("-o writes the result there and leaves the map as it is")
    {
      const auto output = fixture.env.path("out.map");
      const auto json = liveJson({"apply", fixture.map, "-", "-o", arg(output)}, batch);
      const auto fileJson =
        parseOutput(fixture.apply(batch, {"-o", arg(fixture.env.path("file.map"))}));
      CHECK(json["rev"] == fileJson["rev"]);
      CHECK(
        withoutVolatileLines(fixture.env.readFile("out.map"))
        == withoutVolatileLines(fixture.env.readFile("file.map")));
      CHECK(!map.canUndoCommand());
      CHECK(fixture.unchanged());

      // with --dry-run, nothing is written
      const auto dryRun = liveJson(
        {"apply", fixture.map, "-", "--dry-run", "-o", arg(fixture.env.path("dry.map"))},
        batch);
      CHECK(dryRun["rev"] == fileJson["rev"]);
      CHECK(!std::filesystem::exists(fixture.env.path("dry.map")));
    }

    SECTION("--expect-rev takes the live rev")
    {
      checkError(
        editor.run({"apply", fixture.map, "-", "--expect-rev", "live-0:0"}, batch),
        "stale_rev",
        3);
      liveJson({"apply", fixture.map, "-", "--expect-rev", editor.rev()}, batch);
    }
  }

  SECTION("node handles")
  {
    auto& map = editor.map();
    const auto showLive = [&](const std::string& reference) {
      return liveJson({"show", fixture.map, reference})["node"];
    };
    const auto applyLive = [&](const std::string& batch) {
      return liveJson({"apply", fixture.map, "-"}, batch);
    };
    // every path in the output has a handle, which names the node at that path
    const auto checkHandles = [&](const Json& json) {
      const auto pairs = pathsAndHandles(json);
      CHECK(!pairs.empty());
      for (const auto& [path, handle] : pairs)
      {
        CAPTURE(path, handle);
        CHECK_THAT(handle, StartsWith("n:" + editor.epoch() + "."));
        CHECK(showLive(handle)["path"] == Json{path});
      }
    };

    auto* playerStart = map.worldNode().defaultLayer()->children()[2];
    const auto light = showLive("layer:0/entity:2")["handle"].asString();
    const auto deleteLight = R"([{"op": "delete", "target": ")" + light + R"("}])";

    SECTION("every path in a read has a handle next to it")
    {
      checkHandles(liveJson({"describe", fixture.map}));
      checkHandles(liveJson({"query", fixture.map}));
      checkHandles(liveJson({"query", fixture.map, R"(material == "__TB_empty")"}));
      checkHandles(liveJson({"show", fixture.map, "world"}));
      checkHandles(liveJson({"show", fixture.map, "layer:0"}));
      checkHandles(liveJson({"show", fixture.map, "layer:0/brush:0"}));
      checkHandles(liveJson({"show", fixture.map, "layer:0/entity:1"}));

      const auto brush = showLive("layer:0/brush:0");
      CHECK(brush["entity_handle"] == showLive("world")["handle"]);
      CHECK(brush["faces"][2]["handle"] == Json{brush["handle"].asString() + "/face:2"});
    }

    SECTION("every path in an apply result has a handle next to it")
    {
      const auto json = applyLive(R"([
        {"op": "create_box", "min": [0, 0, 0], "max": [64, 32, 16],
         "material": "floor", "as": "$box"},
        {"op": "set_property", "target": "layer:0/entity:1", "key": "speed",
         "value": "50"}
      ])");
      const auto& ops = json["ops"];
      CHECK(ops[0]["created_handles"].size() == 1);
      CHECK(ops[0]["removed_handles"] == Json::array());
      CHECK(json["bindings_handles"]["$box"] == ops[0]["created_handles"][0]);
      CHECK(ops[1]["modified_handles"].size() == 1);
      checkHandles(json);

      // removed paths are those of the input rev, with the handles of their nodes
      const auto removed = applyLive(deleteLight)["ops"][0];
      CHECK(removed["removed"] == Json{Json::Array{"layer:0/entity:2"}});
      CHECK(removed["removed_handles"] == Json{Json::Array{light}});
    }

    SECTION("every path in a verify result has a handle next to it")
    {
      auto entity = std::make_unique<mdl::EntityNode>(
        mdl::Entity{{{"classname", "no_such_class"}, {"origin", "0 0 0"}}});
      auto nodes = std::vector<std::unique_ptr<mdl::Node>>{};
      nodes.push_back(std::move(entity));
      mdl::addNodes(map, *map.worldNode().defaultLayer(), std::move(nodes));

      const auto json = liveJson({"verify", fixture.map});
      REQUIRE(json["issues"]["total"].asNumber() > 0);
      checkHandles(json["issues"]);
    }

    SECTION("an error names its node by handle too")
    {
      const auto result = editor.run(
        {"apply", fixture.map, "-"},
        R"([{"op": "set_property", "target": "layer:0/brush:0", "key": "a",
             "value": "b"}])");
      checkError(result, "wrong_node_kind", 1, 0);
      const auto error = parseOutput(result)["error"];
      CHECK(error["node"] == Json{"layer:0/brush:0"});
      CHECK(error["node_handle"] == showLive("layer:0/brush:0")["handle"]);

      // in file mode, there is none
      CHECK(
        parseOutput(runCli(
          {"apply", fixture.map, "-"},
          R"([{"op": "set_property", "target": "layer:0/brush:0", "key": "a",
               "value": "b"}])"))["error"]
          .find("node_handle")
        == nullptr);
    }

    SECTION("a node keeps its handle across commands")
    {
      const auto queryHandles = [&]() {
        auto result = std::vector<std::string>{};
        for (const auto& pair : pathsAndHandles(liveJson({"query", fixture.map})))
        {
          result.push_back(pair.handle);
        }
        return result;
      };
      const auto before = queryHandles();
      applyLive("[" + std::string{CreateBoxOp} + "]");
      const auto after = queryHandles();
      REQUIRE(after.size() > before.size());
      CHECK(std::vector(after.begin(), after.begin() + long(before.size())) == before);
    }

    SECTION("a handle names its node after the mapper shifted the node's path")
    {
      mdl::removeNodes(map, {playerStart});
      CHECK(showLive(light)["path"] == Json{"layer:0/entity:1"});

      applyLive(
        R"([{"op": "set_property", "target": ")" + light
        + R"(", "key": "light", "value": "250"}])");
      CHECK(showLive("layer:0/entity:1")["properties"]["light"] == Json{"250"});

      // as an op's target flag too
      liveJson(
        {"set_property",
         fixture.map,
         "--target",
         light,
         "--key",
         "light",
         "--value",
         "100"});
      CHECK(showLive(light)["properties"]["light"] == Json{"100"});
    }

    SECTION("a face handle names the face")
    {
      const auto face = showLive("layer:0/brush:1")["faces"][3];
      const auto& handle = face["handle"].asString();
      CHECK(showLive(handle)["path"] == face["path"]);
      CHECK(
        liveJson({"query", fixture.map, "--touching", handle})
        == liveJson({"query", fixture.map, "--touching", face["path"].asString()}));
    }

    SECTION("verify takes a handle as its target")
    {
      // the stages hold timings, which differ from run to run
      const auto byHandle =
        withoutKeys(liveJson({"verify", fixture.map, "--target", light}), {"stages"});
      CHECK(
        byHandle
        == withoutKeys(
          liveJson({"verify", fixture.map, "--target", "layer:0/entity:2"}), {"stages"}));
    }

    SECTION("a deleted node's handle gives gone, and names the node again after undo")
    {
      applyLive(deleteLight);
      checkError(editor.run({"show", fixture.map, light}), "gone", 1);
      checkError(editor.run({"apply", fixture.map, "-"}, deleteLight), "gone", 1, 0);

      map.undoCommand();
      CHECK(showLive(light)["path"] == Json{"layer:0/entity:2"});
    }

    SECTION("a duplicate gets a handle of its own")
    {
      const auto original = showLive("layer:0/entity:0")["handle"];
      mdl::selectNodes(map, {playerStart});
      mdl::duplicateSelectedNodes(map);

      const auto items = liveJson({"query", fixture.map})["items"].asArray();
      const auto copy = items.back();
      CHECK(copy["classname"] == Json{"info_player_start"});
      CHECK(copy["handle"] != original);
      CHECK(showLive(original.asString())["path"] == Json{"layer:0/entity:0"});
    }

    SECTION("a handle from an earlier session gives stale_session")
    {
      auto reopened = FakeEditorHost{fixture.env.path(fixture.fileName)};
      REQUIRE(reopened.epoch() != editor.epoch());
      checkError(reopened.run({"show", fixture.map, light}), "stale_session", 1);
    }

    SECTION("a handle that the session never gave out names nothing")
    {
      checkError(
        editor.run({"show", fixture.map, "n:" + editor.epoch() + ".999"}),
        "node_not_found",
        1);
      checkError(editor.run({"show", fixture.map, "n:nonsense"}), "invalid_path", 1);
    }

    SECTION("file mode reports no handles and takes none")
    {
      const auto describe = runJson({"describe", fixture.map});
      CHECK(describe.find("session") == nullptr);
      CHECK(withoutHandles(describe) == describe);

      const auto brush = runJson({"show", fixture.map, "layer:0/brush:0"});
      CHECK(withoutHandles(brush) == brush);
      checkError(runCli({"show", fixture.map, light}), "invalid_path", 1);
    }
  }

  SECTION("undo")
  {
    auto& map = editor.map();
    const auto box = "[" + std::string{CreateBoxOp} + "]";
    const auto checkRefused = [&]() {
      const auto brushesBefore = liveBrushCount();
      const auto* undoName = map.undoCommandName();
      const auto undoNameBefore = undoName ? std::optional{*undoName} : std::nullopt;
      const auto revBefore = editor.rev();

      checkError(editor.run({"undo", fixture.map}), "undo_refused", 1);

      CHECK(liveBrushCount() == brushesBefore);
      undoName = map.undoCommandName();
      CHECK((undoName ? std::optional{*undoName} : std::nullopt) == undoNameBefore);
      CHECK(editor.rev() == revBefore);
    };

    SECTION("undoes the agent's most recent batch")
    {
      const auto brushesBefore = liveBrushCount();
      const auto applied = liveJson({"apply", fixture.map, "-"}, box);

      const auto json = liveJson({"undo", fixture.map});
      CHECK(json["schema"] == Json{"tbcli.undo/1"});
      CHECK(json["session"] == Json{"live"});
      CHECK(json["undone"] == Json{"AI: create_box"});
      CHECK(json["rev"] == Json{editor.rev()});
      CHECK(liveRevCounter(json) > liveRevCounter(applied));

      CHECK(liveBrushCount() == brushesBefore);
      CHECK(!map.canUndoCommand());
      // it went through the editor's history, so the mapper can redo it
      REQUIRE(map.redoCommandName() != nullptr);
      CHECK(*map.redoCommandName() == "AI: create_box");
      CHECK(fixture.unchanged());
    }

    SECTION("undoes the agent's batches one after another")
    {
      const auto brushesBefore = liveBrushCount();
      liveJson({"apply", fixture.map, "-"}, box);
      liveJson({"apply", fixture.map, "-"}, box);

      liveJson({"undo", fixture.map});
      CHECK(liveBrushCount() == brushesBefore + 1);
      liveJson({"undo", fixture.map});
      CHECK(liveBrushCount() == brushesBefore);
      checkRefused();
    }

    SECTION("a dry run or a failed batch keeps the agent's batch undoable")
    {
      const auto brushesBefore = liveBrushCount();
      liveJson({"apply", fixture.map, "-"}, box);
      liveJson({"apply", fixture.map, "-", "--dry-run"}, box);
      checkError(
        editor.run(
          {"apply", fixture.map, "-"},
          R"([{"op": "set_property", "target": "layer:0/brush:0", "key": "a",
               "value": "b"}])"),
        "wrong_node_kind",
        1,
        0);

      liveJson({"undo", fixture.map});
      CHECK(liveBrushCount() == brushesBefore);
    }

    SECTION("is refused when the mapper changed the map after the batch")
    {
      liveJson({"apply", fixture.map, "-"}, box);
      mdl::removeNodes(map, {map.worldNode().defaultLayer()->children()[2]});
      checkRefused();
    }

    SECTION("is refused when the mapper changed the selection after the batch")
    {
      liveJson({"apply", fixture.map, "-"}, box);
      mdl::selectAllNodes(map);
      checkRefused();
    }

    SECTION("is refused after the mapper undid the batch, and after a redo")
    {
      liveJson({"apply", fixture.map, "-"}, box);
      liveJson({"apply", fixture.map, "-"}, box);

      // the agent's earlier batch is on top now, but the mapper's undo is more recent
      map.undoCommand();
      checkRefused();

      map.redoCommand();
      checkRefused();
    }

    SECTION("is refused when there is nothing to undo")
    {
      checkRefused();
    }

    SECTION("an empty batch is not a batch to undo")
    {
      mdl::removeNodes(map, {map.worldNode().defaultLayer()->children()[2]});
      liveJson({"apply", fixture.map, "-"}, "[]");
      checkRefused();
    }

    SECTION("takes no options")
    {
      liveJson({"apply", fixture.map, "-"}, box);
      checkError(editor.run({"undo", fixture.map, "--dry-run"}), "usage", 2);
      checkError(editor.run({"undo", fixture.map, "extra"}), "usage", 2);
    }
  }

  SECTION("save")
  {
    auto& map = editor.map();
    const auto mapPath = fixture.env.path(fixture.fileName);
    const auto brushesBefore = liveBrushCount();
    liveJson({"apply", fixture.map, "-"}, "[" + std::string{CreateBoxOp} + "]");
    REQUIRE(map.modified());
    const auto revBefore = editor.rev();
    // the brushes in a map file, as tbcli reads it
    const auto fileBrushCount = [&](const std::filesystem::path& path) {
      return runJson({"describe", arg(path)})["counts"]["brushes"].asNumber();
    };

    SECTION("writes the open map to its file")
    {
      const auto json = liveJson({"save", fixture.map});
      CHECK(json["schema"] == Json{"tbcli.save/1"});
      CHECK(json["session"] == Json{"live"});
      CHECK(json["rev"] == Json{revBefore});
      CHECK(json["path"] == Json{fixture.map});
      CHECK(json["modified"] == Json{false});

      CHECK(!map.modified());
      CHECK(fileBrushCount(mapPath) == brushesBefore + 1);
      // a save is not a change to the map, so the agent can still undo its batch
      CHECK(editor.rev() == revBefore);
      liveJson({"undo", fixture.map});
      CHECK(liveBrushCount() == brushesBefore);
    }

    SECTION("--output writes a copy and leaves the map's file and state alone")
    {
      const auto copy = fixture.env.path("copy.map");
      const auto json = liveJson({"save", fixture.map, "--output", arg(copy)});
      CHECK(json["path"] == Json{arg(copy)});
      CHECK(json["modified"] == Json{true});
      CHECK(json["rev"] == Json{revBefore});

      CHECK(fileBrushCount(copy) == brushesBefore + 1);
      CHECK(fixture.unchanged());
      CHECK(map.modified());
      CHECK(map.path() == mapPath);

      const auto other = fixture.env.path("other.map");
      liveJson({"save", fixture.map, "-o", arg(other)});
      CHECK(fileBrushCount(other) == brushesBefore + 1);
      CHECK(fixture.unchanged());
    }

    SECTION("a failed write gives write_failed and leaves the map modified")
    {
      {
        const auto readOnly =
          fs::SetPermissions{mapPath, std::filesystem::perms::owner_read};
        checkError(editor.run({"save", fixture.map}), "write_failed", 1);
      }
      CHECK(map.modified());
      CHECK(fixture.unchanged());

      checkError(
        editor.run(
          {"save", fixture.map, "--output", arg(fixture.env.path("missing/copy.map"))}),
        "write_failed",
        1);
      CHECK(map.modified());
    }

    SECTION("--output cannot name the map's own file")
    {
      checkError(
        editor.run({"save", fixture.map, "--output", fixture.map}),
        "invalid_argument",
        2);
      CHECK(fixture.unchanged());
      CHECK(map.modified());
    }

    SECTION("gives busy while a transaction is open")
    {
      map.startTransaction("mapper drag", mdl::TransactionScope::LongRunning);
      checkError(editor.run({"save", fixture.map}), "busy", 1);
      map.cancelTransaction();
      CHECK(fixture.unchanged());
      CHECK(map.modified());
    }

    SECTION("takes no other options")
    {
      checkError(editor.run({"save", fixture.map, "--dry-run"}), "usage", 2);
      // --output is save's own long form of -o
      checkError(
        editor.run({"apply", fixture.map, "-", "--output", "x.map"}, "[]"), "usage", 2);
      checkError(editor.run({"save", fixture.map, "extra"}), "usage", 2);
      CHECK(map.modified());
    }
  }

  SECTION("view")
  {
    auto& map = editor.map();
    auto* layer = map.worldNode().defaultLayer();
    auto* brush0 = dynamic_cast<mdl::BrushNode*>(layer->children()[0]);
    auto* brush1 = dynamic_cast<mdl::BrushNode*>(layer->children()[1]);
    auto* light = layer->children()[4];
    REQUIRE(brush0 != nullptr);
    REQUIRE(brush1 != nullptr);

    // a 2D view from above at zoom 1 on a screen with two physical pixels per logical
    // pixel: the origin is at the center of the 800x600 logical viewport
    const auto topDown = CameraPose{
      .projection = CameraPose::Projection::Orthographic,
      .position = {0, 0, 100},
      .direction = {0, 0, -1},
      .up = {0, 1, 0},
      .viewport = {800, 600},
    };
    editor.setMapperView(
      ViewCapture{
        .png = "PNG of the mapper's view",
        .image = {1600, 1200},
        .camera = topDown,
        .pixelRatio = 2.0f,
      });

    SECTION("captures the mapper's view and reports what it shows")
    {
      mdl::selectNodes(map, {brush0});
      const auto revBefore = editor.rev();

      const auto json = liveJson({"view", fixture.map});
      CHECK(json["schema"] == Json{"tbcli.view/1"});
      CHECK(json["session"] == Json{"live"});
      CHECK(json["rev"] == Json{revBefore});
      // looking is not a change
      CHECK(editor.rev() == revBefore);

      const auto imagePath = std::filesystem::path{json["image"]["path"].asString()};
      CHECK(readFile(imagePath) == "PNG of the mapper's view");
      CHECK(json["image"]["width"] == Json{1600});
      CHECK(json["image"]["height"] == Json{1200});
      // nothing is written next to the map
      CHECK(imagePath.parent_path() != fixture.env.path(""));
      CHECK(json.find("out") == nullptr);

      CHECK(json["view"] == Json{"xy"});
      CHECK(json["camera"] == toJson(topDown));
      CHECK(json["pixel_ratio"] == Json{2});

      // brush 0 spans -64..64 on x and y, so 128 logical pixels around the center,
      // twice that in the image
      const auto& nodes = json["selection"]["nodes"];
      CHECK(nodes["total"] == Json{1});
      REQUIRE(nodes["items"].size() == 1);
      const auto& selected = nodes["items"][0];
      CHECK(selected["path"] == Json{"layer:0/brush:0"});
      CHECK(selected["handle"] == Json{editor.handles().handle(*brush0)});
      CHECK(selected["box"] == Json{Json::Array{672, 472, 928, 728}});
      CHECK(json["selection"]["faces"]["total"] == Json{0});

      CHECK(json["hovered"] == Json{nullptr});
    }

    SECTION("reports the selected faces")
    {
      mdl::selectBrushFaces(map, {mdl::BrushFaceHandle{*brush1, 3}});

      const auto json = liveJson({"view", fixture.map});
      CHECK(json["selection"]["nodes"]["total"] == Json{0});
      const auto& faces = json["selection"]["faces"];
      REQUIRE(faces["items"].size() == 1);
      CHECK(faces["items"][0]["path"] == Json{"layer:0/brush:1/face:3"});
      CHECK(faces["items"][0]["handle"] == Json{editor.handles().faceHandle(*brush1, 3)});
      CHECK(faces["items"][0]["box"].isArray());
    }

    SECTION("reports the node and face under the mouse")
    {
      auto capture = editor.captureMapperView();
      capture.hovered = HoveredPick{brush1, 3, {0, 0, 128}};
      editor.setMapperView(capture);

      const auto json = liveJson({"view", fixture.map});
      const auto& hovered = json["hovered"];
      CHECK(hovered["path"] == Json{"layer:0/brush:1"});
      CHECK(hovered["handle"] == Json{editor.handles().handle(*brush1)});
      CHECK(hovered["face"] == Json{"layer:0/brush:1/face:3"});
      CHECK(hovered["face_handle"] == Json{editor.handles().faceHandle(*brush1, 3)});
      CHECK(hovered["point"] == Json{Json::Array{0, 0, 128}});
      // brush 1 spans -64..-48 on x and -64..64 on y
      CHECK(hovered["box"] == Json{Json::Array{672, 472, 704, 728}});

      capture.hovered = HoveredPick{light, std::nullopt, {100, 0, 64}};
      editor.setMapperView(capture);
      const auto lightJson = liveJson({"view", fixture.map});
      const auto& hoveredLight = lightJson["hovered"];
      CHECK(hoveredLight["path"] == Json{"layer:0/entity:2"});
      CHECK(hoveredLight.find("face") == nullptr);
    }

    SECTION("a node outside the image has no box")
    {
      mdl::selectNodes(map, {light});
      auto capture = editor.captureMapperView();
      capture.camera.position = {5000, 0, 100};
      editor.setMapperView(capture);

      const auto json = liveJson({"view", fixture.map});
      CHECK(json["selection"]["nodes"]["items"][0]["box"] == Json{nullptr});
    }

    SECTION("--out copies the image")
    {
      const auto out = fixture.env.path("look.png");
      const auto json = liveJson({"view", fixture.map, "--out", arg(out)});
      CHECK(json["out"] == Json{arg(out)});
      CHECK(readFile(out) == "PNG of the mapper's view");
      CHECK(
        std::filesystem::path{json["image"]["path"].asString()}.parent_path()
        != out.parent_path());

      checkError(
        editor.run(
          {"view", fixture.map, "--out", arg(fixture.env.path("missing/look.png"))}),
        "write_failed",
        1);
      checkError(
        editor.run({"view", fixture.map, "--out", fixture.map}), "invalid_argument", 2);
      CHECK(fixture.unchanged());
    }

    SECTION("keeps only the latest images")
    {
      auto paths = std::vector<std::filesystem::path>{};
      for (size_t i = 0; i < KeptViewImages + 5; ++i)
      {
        paths.emplace_back(liveJson({"view", fixture.map})["image"]["path"].asString());
      }

      const auto dir = paths.back().parent_path();
      auto count = size_t{0};
      for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{dir})
      {
        ++count;
      }
      CHECK(count == KeptViewImages);
      CHECK(std::filesystem::exists(paths.back()));
      CHECK(std::filesystem::exists(paths[5]));
      CHECK(!std::filesystem::exists(paths[4]));
      CHECK(!std::filesystem::exists(paths.front()));
    }

    SECTION("gives view_failed if the editor cannot capture its view")
    {
      editor.setMapperView(std::nullopt);
      checkError(editor.run({"view", fixture.map}), "view_failed", 1);
    }

    SECTION("agent cameras")
    {
      // the agent's renders don't use the mapper's view
      editor.setMapperView(std::nullopt);

      const auto vec = [](const Json& json) {
        return vm::vec3f{
          float(json[0].asNumber()),
          float(json[1].asNumber()),
          float(json[2].asNumber())};
      };
      const auto near = [](const vm::vec3f& lhs, const vm::vec3f& rhs) {
        return vm::is_equal(lhs, rhs, 0.001f);
      };

      // the center of everything in the map, as describe reports its bounds
      const auto bounds = liveJson({"describe", fixture.map})["bounds"];
      const auto mapCenter = (vec(bounds["min"]) + vec(bounds["max"])) / 2.0f;

      SECTION("--top, --front and --side look along an axis at the whole map")
      {
        const auto top = liveJson({"view", fixture.map, "--top"});
        CHECK(top["schema"] == Json{"tbcli.view/1"});
        CHECK(top["view"] == Json{"xy"});
        CHECK(top["camera"]["projection"] == Json{"orthographic"});
        CHECK(near(vec(top["camera"]["direction"]), {0, 0, -1}));
        CHECK(top["camera"]["viewport"] == Json{Json::Array{1024, 768}});
        CHECK(top["image"]["width"] == Json{1024});
        CHECK(top["image"]["height"] == Json{768});
        CHECK(readFile(top["image"]["path"].asString()) == "PNG of an agent view");
        CHECK(top["pixel_ratio"] == Json{1});
        CHECK(top["hovered"] == Json{nullptr});

        // centered on the map, above the world
        const auto position = vec(top["camera"]["position"]);
        CHECK(near(
          position, {mapCenter.x(), mapCenter.y(), float(map.worldBounds().max.z())}));

        CHECK(liveJson({"view", fixture.map, "--front"})["view"] == Json{"xz"});
        CHECK(liveJson({"view", fixture.map, "--side"})["view"] == Json{"yz"});
      }

      SECTION("--look-at fits a node or face, by path or handle")
      {
        // brush 0 spans -64..64 on x and y and -16..0 on z
        const auto top = liveJson(
          {"view", fixture.map, "--top", "--look-at", editor.handles().handle(*brush0)});
        CHECK(
          near(vec(top["camera"]["position"]), {0, 0, float(map.worldBounds().max.z())}));

        // a subject alone is orbited from the southwest, above it
        mdl::selectNodes(map, {brush0});
        const auto orbit =
          liveJson({"view", fixture.map, "--look-at", "layer:0/brush:0"});
        CHECK(orbit["view"] == Json{"3d"});
        CHECK(orbit["camera"]["projection"] == Json{"perspective"});
        const auto position = vec(orbit["camera"]["position"]);
        CHECK(position.x() < -64.0f);
        CHECK(position.y() < -64.0f);
        CHECK(position.z() > 0.0f);
        CHECK(near(
          vec(orbit["camera"]["direction"]),
          vm::normalize(vm::vec3f{0, 0, -8} - position)));
        // the selected brush is the subject, so it is well inside the image
        const auto& box = orbit["selection"]["nodes"]["items"][0]["box"];
        CHECK(box[0].asNumber() > 0);
        CHECK(box[1].asNumber() > 0);
        CHECK(box[2].asNumber() < 1024);
        CHECK(box[3].asNumber() < 768);

        // a face of brush 1, whose bottom face spans -64..-48 on x at z 0
        const auto face =
          liveJson({"view", fixture.map, "--top", "--look-at", "layer:0/brush:1/face:2"});
        CHECK(vec(face["camera"]["position"]).x() == -56.0f);
      }

      SECTION("--frame fits a box")
      {
        const auto json =
          liveJson({"view", fixture.map, "--front", "--frame", "0,0,0,100,200,50"});
        CHECK(near(
          vec(json["camera"]["position"]), {50, float(map.worldBounds().min.y()), 25}));
      }

      SECTION("--selection fits the selected nodes or faces")
      {
        // brush 1 spans -64..-48 on x and 0..128 on z
        mdl::selectNodes(map, {brush1});
        const auto nodes = liveJson({"view", fixture.map, "--side", "--selection"});
        CHECK(near(
          vec(nodes["camera"]["position"]), {float(map.worldBounds().max.x()), 0, 64}));

        mdl::deselectAll(map);
        mdl::selectBrushFaces(map, {mdl::BrushFaceHandle{*brush1, 2}});
        const auto faces = liveJson({"view", fixture.map, "--top", "--selection"});
        CHECK(vec(faces["camera"]["position"]).x() == -56.0f);
      }

      SECTION("--orbit puts the camera at the given angles around the subject")
      {
        const auto json = liveJson(
          {"view", fixture.map, "--look-at", "layer:0/brush:0", "--orbit", "0,0,500"});
        CHECK(near(vec(json["camera"]["position"]), {500, 0, -8}));
        CHECK(near(vec(json["camera"]["direction"]), {-1, 0, 0}));

        // around the whole map without a subject
        const auto whole = liveJson({"view", fixture.map, "--orbit", "180,0,500"});
        CHECK(near(vec(whole["camera"]["position"]), mapCenter + vm::vec3f{-500, 0, 0}));
      }

      SECTION("--from and --toward place the camera at a point")
      {
        const auto json =
          liveJson({"view", fixture.map, "--from", "0,0,64", "--toward", "100,0,64"});
        CHECK(json["view"] == Json{"3d"});
        CHECK(vec(json["camera"]["position"]) == vm::vec3f{0, 0, 64});
        CHECK(vec(json["camera"]["direction"]) == vm::vec3f{1, 0, 0});
        CHECK(vec(json["camera"]["up"]) == vm::vec3f{0, 0, 1});
      }

      SECTION("--size sets the image size, fitted into 2048 pixels")
      {
        const auto small = liveJson({"view", fixture.map, "--top", "--size", "640x480"});
        CHECK(small["camera"]["viewport"] == Json{Json::Array{640, 480}});
        CHECK(small["image"]["width"] == Json{640});
        CHECK(small["image"]["height"] == Json{480});

        const auto large =
          liveJson({"view", fixture.map, "--top", "--size", "4096x1024"});
        CHECK(large["image"]["width"] == Json{2048});
        CHECK(large["image"]["height"] == Json{512});
      }

      SECTION("--out copies the image")
      {
        const auto out = fixture.env.path("top.png");
        liveJson({"view", fixture.map, "--top", "--out", arg(out)});
        CHECK(readFile(out) == "PNG of an agent view");
      }

      SECTION("invalid camera arguments give invalid_argument")
      {
        const auto invalid = [&](std::vector<std::string> options) {
          auto args = std::vector<std::string>{"view", fixture.map};
          args.insert(args.end(), options.begin(), options.end());
          INFO(kdl::str_join(options, " "));
          checkError(editor.run(args), "invalid_argument", 2);
        };

        invalid({"--size", "640x480"});
        invalid({"--top", "--size", "640"});
        invalid({"--top", "--size", "0x480"});
        invalid({"--top", "--size", "wide"});
        invalid({"--top", "--side"});
        invalid({"--orbit", "0,0", "--front"});
        invalid({"--look-at", "layer:0/brush:0", "--selection"});
        invalid({"--frame", "0,0,0,1,1,1", "--look-at", "layer:0/brush:0"});
        invalid({"--from", "0,0,0"});
        invalid({"--toward", "0,0,0"});
        invalid({"--from", "1,2,3", "--toward", "1,2,3"});
        invalid({"--from", "0,0,0", "--toward", "1,1"});
        invalid({"--from", "0,0,0", "--toward", "1,0,0", "--selection"});
        invalid({"--orbit", "45"});
        invalid({"--orbit", "45,30,100,7"});
        invalid({"--orbit", "0,95"});
        invalid({"--orbit", "0,0,0"});
        invalid({"--orbit", "a,b"});
        invalid({"--frame", "1,2,3"});
        invalid({"--frame", "10,0,0,0,10,10"});
        // nothing selected
        invalid({"--selection"});
        // an empty layer has nothing to look at
        map.worldNode().addChild(std::make_unique<mdl::LayerNode>(mdl::Layer{"empty"}));
        invalid({"--look-at", "layer:1"});

        CHECK(fixture.unchanged());
      }

      SECTION("--look-at gives the errors of node references")
      {
        checkError(
          editor.run({"view", fixture.map, "--look-at", "layer:0/brush:9"}),
          "node_not_found",
          1);
      }

      SECTION("gives view_failed if the editor cannot render")
      {
        editor.setAgentViewsFail(true);
        checkError(editor.run({"view", fixture.map, "--top"}), "view_failed", 1);
      }

      SECTION("only view takes camera options")
      {
        checkError(editor.run({"describe", fixture.map, "--top"}), "usage", 2);
        checkError(
          editor.run({"query", fixture.map, "--look-at", "layer:0/brush:0"}), "usage", 2);
      }
    }

    SECTION("takes no other options")
    {
      checkError(editor.run({"view", fixture.map, "--dry-run"}), "usage", 2);
      checkError(editor.run({"view", fixture.map, "extra"}), "usage", 2);
      checkError(editor.run({"describe", fixture.map, "--out", "x.png"}), "usage", 2);
    }
  }

  SECTION("shown")
  {
    auto& map = editor.map();
    auto* layer = map.worldNode().defaultLayer();
    auto* brush0 = dynamic_cast<mdl::BrushNode*>(layer->children()[0]);
    auto* brush1 = dynamic_cast<mdl::BrushNode*>(layer->children()[1]);
    REQUIRE(brush0 != nullptr);
    REQUIRE(brush1 != nullptr);

    const auto topDown = CameraPose{
      .projection = CameraPose::Projection::Orthographic,
      .position = {0, 0, 100},
      .direction = {0, 0, -1},
      .up = {0, 1, 0},
      .viewport = {800, 600},
    };
    const auto shownCapture = ViewCapture{
      .png = "PNG of the shown view",
      .image = {800, 600},
      .camera = topDown,
      .hovered = HoveredPick{brush1, 3, {-56, 0, 64}},
      .cursor = ImagePoint{344, 300},
    };
    editor.setMapperView(
      ViewCapture{
        .png = "PNG of the mapper's view",
        .image = {800, 600},
        .camera = topDown,
      });

    SECTION("every live output carries a hint while a shown view is unseen")
    {
      CHECK(liveJson({"describe", fixture.map}).find("shown") == nullptr);

      const auto shownRev = editor.rev();
      editor.show(
        shownCapture, std::chrono::steady_clock::now() - std::chrono::seconds{5});

      const auto hint = liveJson({"describe", fixture.map})["shown"];
      CHECK(hint["rev"] == Json{shownRev});
      CHECK(hint["age"].asNumber() >= 5);
      CHECK(hint["age"].asNumber() < 60);
      CHECK(hint["run"] == Json{"tbcli view"});

      // the hint keeps the rev it was shown at as the map moves on
      const auto applied =
        liveJson({"apply", fixture.map, "-"}, "[" + std::string{CreateBoxOp} + "]");
      CHECK(applied["shown"]["rev"] == Json{shownRev});
      CHECK(liveJson({"query", fixture.map})["shown"]["rev"] == Json{shownRev});

      // and errors carry it too
      const auto failed = editor.run({"show", fixture.map, "layer:0/brush:99"});
      CHECK(failed.exitCode != 0);
      CHECK(parseOutput(failed)["shown"]["rev"] == Json{shownRev});
    }

    SECTION("view returns the shown view once")
    {
      mdl::selectNodes(map, {brush0});
      const auto shownRev = editor.rev();
      editor.show(shownCapture);
      // the shown view keeps what it showed when it was shown
      mdl::deselectAll(map);

      const auto json = liveJson({"view", fixture.map});
      CHECK(
        readFile(std::filesystem::path{json["image"]["path"].asString()})
        == "PNG of the mapper's view");
      CHECK(json.find("shown") != nullptr);

      const auto& shown = json["shown"];
      CHECK(shown["rev"] == Json{shownRev});
      CHECK(shown["age"].isNumber());
      const auto shownImage = std::filesystem::path{shown["image"]["path"].asString()};
      CHECK(readFile(shownImage) == "PNG of the shown view");
      CHECK(shownImage != std::filesystem::path{json["image"]["path"].asString()});
      CHECK(shown["image"]["width"] == Json{800});
      CHECK(shown["image"]["height"] == Json{600});
      CHECK(shown["view"] == Json{"xy"});
      CHECK(shown["camera"] == toJson(topDown));
      CHECK(shown["pixel_ratio"] == Json{1});
      CHECK(shown["cursor"] == Json{Json::Array{344, 300}});

      const auto& pick = shown["hovered"];
      CHECK(pick["path"] == Json{"layer:0/brush:1"});
      CHECK(pick["handle"] == Json{editor.handles().handle(*brush1)});
      CHECK(pick["face"] == Json{"layer:0/brush:1/face:3"});
      CHECK(pick["face_handle"] == Json{editor.handles().faceHandle(*brush1, 3)});
      // brush 1 spans -64..-48 on x and -64..64 on y
      CHECK(pick["box"] == Json{Json::Array{336, 236, 352, 364}});

      const auto& selected = shown["selection"]["nodes"]["items"];
      REQUIRE(selected.size() == 1);
      CHECK(selected[0]["path"] == Json{"layer:0/brush:0"});
      CHECK(json["selection"]["nodes"]["total"] == Json{0});

      // seen once: the hint and the shown view are gone
      CHECK(liveJson({"describe", fixture.map}).find("shown") == nullptr);
      CHECK(liveJson({"view", fixture.map}).find("shown") == nullptr);
    }

    SECTION("view's options don't matter")
    {
      editor.show(shownCapture);
      const auto out = fixture.env.path("look.png");
      const auto json = liveJson({"view", fixture.map, "--out", arg(out)});
      CHECK(json["shown"]["cursor"] == Json{Json::Array{344, 300}});
      CHECK(readFile(out) == "PNG of the mapper's view");
    }

    SECTION("an agent camera's view returns and takes it too")
    {
      editor.show(shownCapture);
      const auto json = liveJson({"view", fixture.map, "--top"});
      CHECK(json["view"] == Json{"xy"});
      CHECK(json["shown"]["cursor"] == Json{Json::Array{344, 300}});
      CHECK(editor.shownView() == nullptr);
    }

    SECTION("a failed view leaves the shown view for the next one")
    {
      editor.show(shownCapture);
      editor.setMapperView(std::nullopt);
      const auto failed = editor.run({"view", fixture.map});
      CHECK(failed.exitCode == 1);
      CHECK(parseOutput(failed)["shown"]["run"] == Json{"tbcli view"});
    }

    SECTION("a second show replaces the first")
    {
      editor.show(shownCapture);
      auto second = shownCapture;
      second.png = "PNG of the second shown view";
      second.cursor = std::nullopt;
      second.hovered = std::nullopt;
      editor.show(second);

      const auto shown = liveJson({"view", fixture.map})["shown"];
      CHECK(
        readFile(std::filesystem::path{shown["image"]["path"].asString()})
        == "PNG of the second shown view");
      CHECK(shown["cursor"] == Json{nullptr});
      CHECK(shown["hovered"] == Json{nullptr});
      CHECK(liveJson({"view", fixture.map}).find("shown") == nullptr);
    }

    SECTION("file mode has no hint")
    {
      editor.show(shownCapture);
      CHECK(runJson({"describe", fixture.map}).find("shown") == nullptr);
    }
  }

  SECTION("changes")
  {
    auto& map = editor.map();
    auto* layer = map.worldNode().defaultLayer();
    auto* playerStart = layer->children()[2];
    auto* light = layer->children()[4];
    const auto showLive = [&](const std::string& reference) {
      return liveJson({"show", fixture.map, reference})["node"];
    };
    const auto lightHandle = showLive("layer:0/entity:2")["handle"].asString();
    const auto playerStartHandle = showLive("layer:0/entity:0")["handle"].asString();
    const auto changesSince = [&](const std::string& rev) {
      return liveJson({"changes", fixture.map, "--since", rev});
    };
    const auto setLight = [&](const std::string& value) {
      mdl::deselectAll(map);
      mdl::selectNodes(map, {light});
      // the edit is the most recent undo step
      REQUIRE(mdl::setEntityProperty(map, "light", value));
    };
    const auto change = [](
                          const std::string& kind,
                          const std::string& handle,
                          const std::optional<std::string>& path,
                          const std::string& origin) {
      auto result = Json::object();
      result.set("kind", kind);
      if (path)
      {
        result.set("path", *path);
      }
      result.set("handle", handle);
      result.set("origin", origin);
      return result;
    };
    // the changes without their revs, which the test doesn't predict
    const auto withoutRevs = [](const Json& changes) {
      auto result = Json::array();
      for (const auto& entry : changes.asArray())
      {
        result.push(withoutKeys(entry, {"rev"}));
      }
      return result;
    };

    const auto rev0 = editor.rev();

    SECTION("lists the nodes created, modified and removed since the rev, by whom")
    {
      setLight("200");
      const auto created =
        liveJson(
          {"apply", fixture.map, "-"},
          "[" + std::string{CreateBoxOp} + "]")["ops"][0]["created_handles"][0]
          .asString();
      mdl::removeNodes(map, {playerStart});

      const auto json = changesSince(rev0);
      CHECK(json["schema"] == Json{"tbcli.changes/1"});
      CHECK(json["session"] == Json{"live"});
      CHECK(json["rev"] == Json{editor.rev()});
      CHECK(json["since"] == Json{rev0});

      const auto& changes = json["changes"];
      CHECK(
        withoutRevs(changes)
        == Json{Json::Array{
          change("modified", lightHandle, "layer:0/entity:1", "mapper"),
          change("created", created, "layer:0/brush:2", "agent"),
          change("removed", playerStartHandle, std::nullopt, "mapper"),
        }});
      // the rev of each node's latest change, in order
      CHECK(
        liveRevCounter(changes[0]) > liveRevCounter(Json{Json::Object{{"rev", rev0}}}));
      CHECK(liveRevCounter(changes[1]) > liveRevCounter(changes[0]));
      CHECK(changes[2]["rev"] == Json{editor.rev()});

      // only what changed after the given rev
      CHECK(
        withoutRevs(changesSince(changes[1]["rev"].asString())["changes"])
        == Json{
          Json::Array{change("removed", playerStartHandle, std::nullopt, "mapper")}});
      CHECK(changesSince(editor.rev())["changes"] == Json::array());
    }

    SECTION("flags the changes of undo and redo, the agent's own undo too")
    {
      setLight("200");
      const auto modifiedLight = [&](const std::string& origin) {
        return change("modified", lightHandle, "layer:0/entity:2", origin);
      };

      map.undoCommand();
      auto undone = modifiedLight("mapper");
      undone.set("undo", true);
      CHECK(withoutRevs(changesSince(rev0)["changes"]) == Json{Json::Array{undone}});

      map.redoCommand();
      auto redone = modifiedLight("mapper");
      redone.set("redo", true);
      CHECK(withoutRevs(changesSince(rev0)["changes"]) == Json{Json::Array{redone}});

      const auto rev1 = editor.rev();
      liveJson(
        {"apply", fixture.map, "-"},
        R"([{"op": "set_property", "target": ")" + lightHandle
          + R"(", "key": "light", "value": "300"}])");
      liveJson({"undo", fixture.map});
      auto agentUndo = modifiedLight("agent");
      agentUndo.set("undo", true);
      CHECK(withoutRevs(changesSince(rev1)["changes"]) == Json{Json::Array{agentUndo}});
    }

    SECTION("leaves out nodes that came and went, and requests that changed nothing")
    {
      const auto box = "[" + std::string{CreateBoxOp} + "]";
      liveJson({"apply", fixture.map, "-"}, box);
      map.undoCommand();
      liveJson({"apply", fixture.map, "-", "--dry-run"}, box);
      liveJson({"apply", fixture.map, "-", "-o", arg(fixture.env.path("out.map"))}, box);
      checkError(
        editor.run(
          {"apply", fixture.map, "-"},
          R"([{"op": "set_property", "target": ")" + lightHandle
            + R"(", "key": "light", "value": "300"},
               {"op": "set_property", "target": "layer:0/brush:0", "key": "a",
                "value": "b"}])"),
        "wrong_node_kind",
        1,
        1);

      CHECK(changesSince(rev0)["changes"] == Json::array());
    }

    SECTION("is too_old past the change log, and refuses revs it can't place")
    {
      auto small = FakeEditorHost{fixture.env.path(fixture.fileName), 2};
      const auto smallRev0 = small.rev();
      const auto smallJson = [&](const std::vector<std::string>& args) {
        const auto result = small.run(args);
        REQUIRE(result.exitCode == 0);
        return parseOutput(result);
      };
      // the second change removes two nodes, which pushes the first one out of the log
      auto* smallLayer = small.map().worldNode().defaultLayer();
      mdl::removeNodes(small.map(), {smallLayer->children()[2]});
      mdl::removeNodes(
        small.map(), {smallLayer->children()[0], smallLayer->children()[2]});
      const auto smallRev1 = small.rev();
      mdl::removeNodes(small.map(), {smallLayer->children()[0]});

      checkError(small.run({"changes", fixture.map, "--since", smallRev0}), "too_old", 1);
      CHECK(
        smallJson({"changes", fixture.map, "--since", smallRev1})["changes"].size() == 1);

      checkError(
        editor.run({"changes", fixture.map, "--since", smallRev1}), "stale_session", 1);
      checkError(
        editor.run({"changes", fixture.map, "--since", "live-" + editor.epoch() + ":99"}),
        "invalid_argument",
        2);
      checkError(
        editor.run({"changes", fixture.map, "--since", revOf(fixture.map)}),
        "invalid_argument",
        2);
      checkError(editor.run({"changes", fixture.map}), "usage", 2);
    }
  }

  SECTION("apply at an old rev")
  {
    auto& map = editor.map();
    auto* layer = map.worldNode().defaultLayer();
    auto* brush1 = layer->children()[1];
    auto* playerStart = layer->children()[2];
    auto* door = layer->children()[3];
    auto* doorBrush = door->children()[0];
    auto* light = layer->children()[4];
    const auto handleAt = [&](const std::string& path) {
      return liveJson({"show", fixture.map, path})["node"]["handle"].asString();
    };
    const auto brush0Handle = handleAt("layer:0/brush:0");
    const auto doorHandle = handleAt("layer:0/entity:1");
    const auto doorBrushHandle = handleAt("layer:0/entity:1/brush:0");
    const auto lightHandle = handleAt("layer:0/entity:2");
    const auto rev0 = editor.rev();

    const auto setProperty = [](const std::string& target) {
      return R"({"op": "set_property", "target": ")" + target
             + R"(", "key": "_tb_test", "value": "1"})";
    };
    const auto translate = [](const std::string& target) {
      return R"({"op": "translate", "target": ")" + target + R"(", "delta": [0, 0, 8]})";
    };
    const auto applyAt = [&](const std::string& rev, const std::string& op) {
      return editor.run({"apply", fixture.map, "-", "--expect-rev", rev}, "[" + op + "]");
    };
    const auto mapperEdits = [&](mdl::Node* node) {
      mdl::deselectAll(map);
      mdl::selectNodes(map, {node});
      REQUIRE(mdl::translateSelection(map, vm::vec3d{0, 0, 16}));
      mdl::deselectAll(map);
    };
    const auto checkStaleRev = [&](const RunResult& result, const Json& conflicts) {
      INFO(result.out);
      CHECK(result.exitCode == 3);
      const auto error = parseOutput(result)["error"];
      CHECK(error["code"] == Json{"stale_rev"});
      CHECK(error["conflicts"] == conflicts);
      // the same summary as changes --since
      CHECK(
        error["changes"]
        == liveJson({"changes", fixture.map, "--since", rev0})["changes"]);
    };
    const auto conflict = [](
                            const std::string& node,
                            const std::optional<std::string>& path,
                            const std::string& reason) {
      auto result = Json::object();
      result.set("op_index", 0);
      result.set("node", node);
      if (path)
      {
        result.set("path", *path);
      }
      result.set("reason", reason);
      return result;
    };

    SECTION("takes handles whose nodes the mapper's edits didn't touch")
    {
      // a sibling, hiding and showing, and selecting don't touch the target
      mapperEdits(brush1);
      mdl::hideNodes(map, {layer->children()[0]});
      mdl::showAllNodes(map);
      mdl::selectNodes(map, {layer->children()[0]});
      REQUIRE(editor.rev() != rev0);

      const auto result = applyAt(rev0, translate(brush0Handle));
      INFO(result.out);
      CHECK(result.exitCode == 0);
      CHECK(*map.undoCommandName() == "AI: translate");
    }

    SECTION("refuses handles whose nodes the mapper's edits touched")
    {
      SECTION("the node itself")
      {
        mapperEdits(light);
        checkStaleRev(
          applyAt(rev0, setProperty(lightHandle)),
          Json{Json::Array{conflict(lightHandle, "layer:0/entity:2", "changed")}});
      }

      SECTION("an ancestor")
      {
        mdl::deselectAll(map);
        mdl::selectNodes(map, {door});
        REQUIRE(mdl::setEntityProperty(map, "speed", "50"));
        checkStaleRev(
          applyAt(rev0, translate(doorBrushHandle)),
          Json{Json::Array{
            conflict(doorBrushHandle, "layer:0/entity:1/brush:0", "ancestor_changed")}});
      }

      SECTION("a descendant of an entity")
      {
        // moving the door's only brush would move the door too
        REQUIRE(
          mdl::setBrushFaceAttributes(
            map,
            {mdl::BrushFaceHandle{*dynamic_cast<mdl::BrushNode*>(doorBrush), 0}},
            mdl::UpdateBrushFaceAttributes{.materialName = "door"}));
        checkStaleRev(
          applyAt(rev0, setProperty(doorHandle)),
          Json{
            Json::Array{conflict(doorHandle, "layer:0/entity:1", "descendant_changed")}});
      }

      SECTION("a group, but not the group's other members, and a layer lock")
      {
        mdl::deselectAll(map);
        mdl::selectNodes(map, {layer->children()[0], brush1});
        auto* group = mdl::groupSelectedNodes(map, "g");
        REQUIRE(group != nullptr);
        mdl::deselectAll(map);
        const auto groupHandle = handleAt("layer:0/group:0");
        const auto memberHandle = handleAt("layer:0/group:0/brush:0");
        const auto revGrouped = editor.rev();

        REQUIRE(
          mdl::setBrushFaceAttributes(
            map,
            {mdl::BrushFaceHandle{
              *dynamic_cast<mdl::BrushNode*>(group->children()[1]), 0}},
            mdl::UpdateBrushFaceAttributes{.materialName = "wall"}));

        const auto dryRun = editor.run(
          {"apply", fixture.map, "-", "--dry-run", "--expect-rev", revGrouped},
          "[" + translate(memberHandle) + "]");
        INFO(dryRun.out);
        CHECK(dryRun.exitCode == 0);
        CHECK(
          parseOutput(applyAt(revGrouped, translate(groupHandle)))["error"]["conflicts"]
          == Json{
            Json::Array{conflict(groupHandle, "layer:0/group:0", "descendant_changed")}});

        mdl::lockNodes(map, {layer});
        CHECK(
          parseOutput(applyAt(revGrouped, translate(memberHandle)))["error"]["conflicts"]
          == Json{Json::Array{
            conflict(memberHandle, "layer:0/group:0/brush:0", "ancestor_changed")}});
      }

      SECTION("a removed node")
      {
        mdl::removeNodes(map, {light});
        checkStaleRev(
          applyAt(rev0, setProperty(lightHandle)),
          Json{Json::Array{conflict(lightHandle, std::nullopt, "removed")}});
      }

      SECTION("names every conflict, with the op that has it")
      {
        mapperEdits(light);
        const auto result = editor.run(
          {"apply", fixture.map, "-", "--expect-rev", rev0},
          "[" + translate(brush0Handle) + ", " + setProperty(lightHandle) + "]");
        auto expected = conflict(lightHandle, "layer:0/entity:2", "changed");
        expected.set("op_index", 1);
        checkStaleRev(result, Json{Json::Array{expected}});
      }
    }

    SECTION("refuses paths unless the rev is the current one")
    {
      mapperEdits(brush1);
      checkStaleRev(
        applyAt(rev0, translate("layer:0/brush:0")),
        Json{Json::Array{conflict("layer:0/brush:0", std::nullopt, "path")}});

      const auto result = applyAt(editor.rev(), translate("layer:0/brush:0"));
      CHECK(result.exitCode == 0);
    }

    SECTION("re-runs queries against the current map, checked by their expect")
    {
      mdl::removeNodes(map, {playerStart});
      const auto query = [](const std::string& expect) {
        return R"({"op": "set_property", "target": {"query": "classname == \"light\"",
                   "expect": )"
               + expect + R"(}, "key": "light", "value": "250"})";
      };

      const auto result = applyAt(rev0, query("1"));
      INFO(result.out);
      CHECK(result.exitCode == 0);
      CHECK(
        liveJson({"show", fixture.map, lightHandle})["node"]["properties"]["light"]
        == Json{"250"});
      checkError(applyAt(rev0, query("2")), "expect_mismatch", 1, 0);
    }

    SECTION("is strict when the rev is older than the change log or not this session's")
    {
      auto small = FakeEditorHost{fixture.env.path(fixture.fileName), 1};
      const auto smallLight = parseOutput(small.run(
        {"show", fixture.map, "layer:0/entity:2"}))["node"]["handle"]
                                .asString();
      const auto smallRev0 = small.rev();
      auto* smallLayer = small.map().worldNode().defaultLayer();
      mdl::removeNodes(
        small.map(), {smallLayer->children()[0], smallLayer->children()[1]});

      const auto result = small.run(
        {"apply", fixture.map, "-", "--expect-rev", smallRev0},
        "[" + setProperty(smallLight) + "]");
      checkError(result, "stale_rev", 3);

      checkError(applyAt(smallRev0, setProperty(lightHandle)), "stale_rev", 3);
      checkError(applyAt("live-0:0", setProperty(lightHandle)), "stale_rev", 3);
    }
  }

  SECTION("live-rev prints the live rev")
  {
    liveJson({"apply", fixture.map, "-"}, "[]");
    CHECK(
      liveJson({std::string{LiveRevCommand}, fixture.map})
      == Json{Json::Object{{"rev", editor.rev()}}});
  }

  SECTION("live-only commands give no_session in file mode")
  {
    checkError(runCli({"undo", fixture.map}), "no_session", 1);
    checkError(runCli({"save", fixture.map}), "no_session", 1);
    checkError(runCli({"view", fixture.map}), "no_session", 1);
    checkError(runCli({"changes", fixture.map, "--since", "live-0:0"}), "no_session", 1);
    checkError(runCli({std::string{LiveRevCommand}, fixture.map}), "no_session", 1);
    CHECK(fixture.unchanged());
  }

  SECTION("game config and FGD overrides are refused")
  {
    checkError(
      editor.run({"describe", fixture.map, "--fgd", arg(quakeFgdPath())}),
      "invalid_argument",
      2);
    checkError(
      editor.run(
        {"apply", fixture.map, "-", "--game-config", arg(quakeGameConfigPath())}, "[]"),
      "invalid_argument",
      2);
  }

  SECTION("a command on another map gives no_session")
  {
    const auto other = arg(fixture.env.copyFixture("maps/simple.map", "other.map"));
    checkError(editor.run({"describe", other}), "no_session", 1);
  }

  SECTION("commands without a map run as in file mode")
  {
    CHECK(editor.run({"ops"}).out == runCli({"ops"}).out);
  }
}

TEST_CASE("runOnEject")
{
  // a compilable map whose qbsp is the fake compiler, open in the editor
  auto env = CliTestEnvironment{};
  auto compilers = Json::object();
  compilers.set(
    std::string{QbspTool}, arg(setUpFakeCompiler(env, QbspTool, "rooms_prt1")));
  auto config = quakeConfig();
  config.set("compilers", std::move(compilers));
  env.writeConfig({}, config);
  const auto map = arg(env.copyFixture("compiled/rooms_prt1.map", "rooms_prt1.map"));
  auto editor = FakeEditorHost{map};

  const auto args = GENERATE(
    std::vector<std::string>{"verify", "--compile"}, std::vector<std::string>{"rooms"});
  auto command = args;
  command.insert(command.begin() + 1, map);
  INFO(command.front());

  const auto ejectOpenMap = [&]() {
    const auto result = editor.run(command);
    INFO(result.out);
    REQUIRE(result.exitCode == 0);
    CHECK(result.out.empty());
    REQUIRE(result.eject.has_value());
    return *result.eject;
  };
  const auto liveRev = [&]() { return std::optional{LiveRevReply{editor.rev()}}; };
  const auto ejectedJson = [&](const Eject& eject, const LiveRevReader& rev) {
    const auto result = runOnEject(command, eject, rev);
    INFO(result.out);
    INFO(result.err);
    REQUIRE(result.exitCode == 0);
    return parseOutput(result);
  };
  // file mode on the same bytes, whose line numbers the output reports
  const auto runFileMode = [&](const Eject& eject) {
    env.writeFile("rooms_prt1.map", eject.bytes);
    return runCli(command);
  };
  const auto withoutRevAndTimings = [](const RunResult& result) {
    return withoutKeys(parseOutput(result), {"rev", "stages"});
  };
  const auto moveBrush = [&]() {
    const auto result =
      editor.run({"translate", map, "--target", "layer:0/brush:0", "--delta", "0,0,8"});
    REQUIRE(result.exitCode == 0);
  };

  SECTION("the editor ejects the open map at its live rev instead of compiling")
  {
    // the open map differs from its file
    moveBrush();

    const auto eject = ejectOpenMap();
    CHECK(eject.path == map);
    CHECK(eject.rev == editor.rev());

    auto stream = std::ostringstream{};
    editor.map().saveTo(stream);
    CHECK(eject.bytes == stream.str());
    CHECK(env.readFile("rooms_prt1.map") != eject.bytes);
  }

  SECTION("the command runs on the ejected bytes")
  {
    moveBrush();
    const auto eject = ejectOpenMap();
    ejectedJson(eject, liveRev);
    CHECK(env.readFile("tools/qbsp.input") == eject.bytes);
  }

  SECTION("output matches file mode on the same bytes, at the rev of the bytes")
  {
    moveBrush();
    const auto eject = ejectOpenMap();
    const auto live = runOnEject(command, eject, liveRev);
    CHECK(parseOutput(live)["rev"] == Json{eject.rev});
    CHECK(parseOutput(live).find("stale") == nullptr);

    const auto fileMode = runFileMode(eject);
    CHECK(live.exitCode == fileMode.exitCode);
    CHECK(withoutRevAndTimings(live) == withoutRevAndTimings(fileMode));
  }

  SECTION("the result is stale if the map moved on during the compile")
  {
    const auto eject = ejectOpenMap();
    // tbcli reads the live rev once the compile is done, by when the mapper edited
    const auto live = ejectedJson(eject, [&]() {
      moveBrush();
      return std::optional{LiveRevReply{editor.rev()}};
    });
    CHECK(live["rev"] == Json{eject.rev});
    CHECK(live["stale"] == Json{true});
  }

  SECTION("the result is stale if the live rev can't be read afterwards")
  {
    const auto eject = ejectOpenMap();
    const auto live = ejectedJson(eject, []() { return std::optional<LiveRevReply>{}; });
    CHECK(live["stale"] == Json{true});
  }

  SECTION("the result carries the shown hint that the editor reports afterwards")
  {
    const auto eject = ejectOpenMap();
    CHECK(ejectedJson(eject, liveRev).find("shown") == nullptr);

    const auto hint =
      Json{Json::Object{{"rev", "live-x:1"}, {"age", 3}, {"run", "tbcli view"}}};
    const auto live = ejectedJson(
      eject, [&]() { return std::optional{LiveRevReply{editor.rev(), hint}}; });
    CHECK(live["shown"] == hint);

    // and so do failed compiles, which rooms reports as errors
    std::filesystem::remove(env.path("tools") / "qbsp.replay");
    const auto failed = runOnEject(
      command, eject, [&]() { return std::optional{LiveRevReply{editor.rev(), hint}}; });
    CHECK(parseOutput(failed)["shown"] == hint);
  }

  SECTION("a failed compile reports its error, as in file mode")
  {
    std::filesystem::remove(env.path("tools") / "qbsp.replay");
    const auto eject = ejectOpenMap();
    const auto live = runOnEject(command, eject, liveRev);
    const auto fileMode = runFileMode(eject);
    CHECK(live.exitCode == fileMode.exitCode);
    CHECK(withoutRevAndTimings(live) == withoutRevAndTimings(fileMode));
  }
}

TEST_CASE("mutatingCommand")
{
  CHECK(mutatingCommand({"apply", "x.map", "-"}) == "apply");
  CHECK(mutatingCommand({"--format", "text", "translate", "x.map"}) == "translate");
  CHECK(mutatingCommand({"undo", "x.map"}) == "undo");
  CHECK(mutatingCommand({"save", "x.map"}) == "save");
  CHECK(mutatingCommand({"describe", "x.map"}) == std::nullopt);
  CHECK(mutatingCommand({"apply", "x.map", "-", "--help"}) == std::nullopt);
  CHECK(mutatingCommand({"apply", "x.map", "--frobnicate"}) == std::nullopt);
  CHECK(mutatingCommand({}) == std::nullopt);
}

TEST_CASE("liveMapArgument")
{
  const auto cwd = std::filesystem::current_path();
  CHECK(liveMapArgument({"describe", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"apply", "--dry-run", "maps/../x.map", "-"}) == cwd / "x.map");
  CHECK(liveMapArgument({"set_property", "x.map", "--target", "world"}) == cwd / "x.map");
  CHECK(liveMapArgument({"verify", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"undo", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"save", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"view", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"changes", "x.map", "--since", "live-ab12:3"}) == cwd / "x.map");
  CHECK(liveMapArgument({"ops"}) == std::nullopt);
  CHECK(liveMapArgument({"rooms", "x.map"}) == cwd / "x.map");
  CHECK(liveMapArgument({"verify", "x.map", "--compile"}) == cwd / "x.map");
  CHECK(liveMapArgument({"guide", "conventions", "x.map"}) == std::nullopt);
  CHECK(liveMapArgument({"describe", "x.map", "--help"}) == std::nullopt);
  CHECK(liveMapArgument({"describe", "x.map", "--frobnicate"}) == std::nullopt);
  CHECK(liveMapArgument({}) == std::nullopt);
}

TEST_CASE("forwardableInvocation")
{
  auto env = CliTestEnvironment{};
  const auto absolute = [](const std::filesystem::path& path) {
    return arg(std::filesystem::absolute(path).lexically_normal());
  };

  SECTION("the map path is made absolute")
  {
    const auto invocation = forwardableInvocation({"describe", "x.map"}, "");
    CHECK(invocation.args == std::vector<std::string>{"describe", absolute("x.map")});
  }

  SECTION("a batch file is read and passed as stdin")
  {
    const auto batch = env.writeFile("batch.json", "[]");
    const auto invocation =
      forwardableInvocation({"apply", "x.map", arg(batch), "--dry-run"}, "");
    CHECK(
      invocation.args
      == std::vector<std::string>{"apply", absolute("x.map"), "-", "--dry-run"});
    CHECK(invocation.in == "[]");
  }

  SECTION("a batch on stdin is passed on")
  {
    const auto invocation = forwardableInvocation({"apply", "x.map", "-"}, "[1]");
    CHECK(invocation.args == std::vector<std::string>{"apply", absolute("x.map"), "-"});
    CHECK(invocation.in == "[1]");
  }

  SECTION("a missing batch file fails with file_not_found")
  {
    try
    {
      forwardableInvocation({"apply", "x.map", arg(env.path("missing.json"))}, "");
      FAIL("expected a CliError");
    }
    catch (const CliError& e)
    {
      CHECK(e.code() == ErrorCode::FileNotFound);
    }
  }

  SECTION("options that name files are made absolute")
  {
    const auto invocation = forwardableInvocation(
      {"apply",
       "x.map",
       "-",
       "-o",
       "out.map",
       "--fgd=defs/quake.fgd",
       "--game-config",
       "Quake.cfg",
       "--quake-dir",
       "quake"},
      "");
    CHECK(
      invocation.args
      == std::vector<std::string>{
        "apply",
        absolute("x.map"),
        "-",
        "-o",
        absolute("out.map"),
        "--fgd=" + absolute("defs/quake.fgd"),
        "--game-config",
        absolute("Quake.cfg"),
        "--quake-dir",
        absolute("quake")});

    CHECK(
      forwardableInvocation({"save", "x.map", "--output", "out.map"}, "").args
      == std::vector<std::string>{
        "save", absolute("x.map"), "--output", absolute("out.map")});
    CHECK(
      forwardableInvocation({"save", "x.map", "--output=out.map"}, "").args
      == std::vector<std::string>{
        "save", absolute("x.map"), "--output=" + absolute("out.map")});
    CHECK(
      forwardableInvocation({"view", "x.map", "--out", "look.png"}, "").args
      == std::vector<std::string>{
        "view", absolute("x.map"), "--out", absolute("look.png")});
  }

  SECTION("compilers are made absolute unless they are looked up in PATH")
  {
    const auto invocation = forwardableInvocation(
      {"verify", "x.map", "--compile", "--qbsp", "qbsp", "--vis", "tools/vis"}, "");
    CHECK(
      invocation.args
      == std::vector<std::string>{
        "verify",
        absolute("x.map"),
        "--compile",
        "--qbsp",
        "qbsp",
        "--vis",
        absolute("tools/vis")});
  }

  SECTION("commands without a map are passed on as they are")
  {
    CHECK(
      forwardableInvocation({"guide", "verify"}, "").args
      == std::vector<std::string>{"guide", "verify"});
  }
}

TEST_CASE("readsStdin")
{
  CHECK(readsStdin({"apply", "x.map", "-"}));
  CHECK(readsStdin({"apply", "--dry-run", "x.map", "-", "-o", "out.map"}));
  CHECK(!readsStdin({"apply", "x.map", "batch.json"}));
  CHECK(!readsStdin({"apply", "x.map", "batch.json", "-o", "-"}));
  CHECK(!readsStdin({"create_box", "x.map", "--material", "-"}));
  CHECK(!readsStdin({"apply", "x.map", "-", "--help"}));
  CHECK(!readsStdin({"apply", "x.map", "-", "--frobnicate"}));
  CHECK(!readsStdin({}));
}

} // namespace tb::cli
