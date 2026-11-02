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
#include "cli/Compile.h"
#include "cli/FileUtils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::cli
{
using namespace std::chrono_literals;
using namespace Catch::Matchers;

namespace
{

std::vector<std::string> messagesOf(const std::vector<CompilerMessage>& messages)
{
  auto result = std::vector<std::string>{};
  for (const auto& message : messages)
  {
    result.push_back(message.message);
  }
  return result;
}

std::string recordedLog(const std::string& recording)
{
  return *readFile(fixturePath(
    std::filesystem::path{"compiled"} / recording / "qbsp" / (recording + ".log")));
}

} // namespace

TEST_CASE("Compile")
{
  SECTION("parseCompilerLog")
  {
    SECTION("keeps the severity-marked lines of a recorded log")
    {
      const auto messages = parseCompilerLog(recordedLog("compile_warnings"), {});
      CHECK(
        messagesOf(messages)
        == std::vector<std::string>{
          "No wad or _wad key exists in the worldmodel",
          "compile_warnings.map[line 92]: brush has multiple face contents (WATER | "
          "TRANSLUCENT | DETAIL vs SOLID), the former will be used.",
          "unable to find texture skip",
          "unable to find texture ground1_6",
          "unable to find texture ceiling1_1",
          "unable to find texture wall1_1",
          "unable to find texture *water1",
          "info_null at -192 64 32.5 has long key "
          "a_property_key_that_is_much_too_long_for_quake (length 46 >= 31)",
        });
      for (const auto& message : messages)
      {
        CHECK(message.severity == Severity::Warning);
      }

      CHECK(messages[0].line == std::nullopt);
      CHECK(messages[0].classname == std::nullopt);

      // qbsp names lines as <map as given>[line N]
      CHECK(messages[1].line == 92);
      CHECK(messages[1].classname == std::nullopt);

      // and entities as <classname> at <origin>, in the numbers' shortest form
      CHECK(messages[7].line == std::nullopt);
      CHECK(messages[7].classname == "info_null");
      CHECK(messages[7].origin == vm::vec3d{-192, 64, 32.5});
    }

    SECTION("takes the line after the error banner as an error")
    {
      const auto messages = parseCompilerLog(recordedLog("compile_fatal"), {});
      REQUIRE(messages.size() == 2);
      CHECK(messages[1].severity == Severity::Error);
      CHECK(
        messages[1].message
        == "ParseBrush: Region brush at compile_fatal.map[line 105] isn't part of the "
           "world entity");
      CHECK(messages[1].line == 105);
      // "brush at compile_fatal.map[line 105]" is not an origin
      CHECK(messages[1].classname == std::nullopt);
    }

    SECTION("takes ERROR: lines as errors and drops other lines")
    {
      const auto messages = parseCompilerLog(
        "INFO: using built-in palette.\n"
        "     9 brushes\r\n"
        "ERROR: something failed\r\n"
        "WARNING:no space\n",
        {});
      REQUIRE(messages.size() == 1);
      CHECK(messages[0].severity == Severity::Error);
      CHECK(messages[0].message == "something failed");
    }

    SECTION("reads origins in parentheses and brackets")
    {
      const auto messages = parseCompilerLog(
        "WARNING: Reached occupant \"info_player_start\" at (-128 0 24), no filling "
        "performed.\n"
        "WARNING: unknown delay 7 on light at [1.5 -2 3e2]\n",
        {});
      REQUIRE(messages.size() == 2);
      CHECK(messages[0].classname == "info_player_start");
      CHECK(messages[0].origin == vm::vec3d{-128, 0, 24});
      CHECK(messages[1].classname == "light");
      CHECK(messages[1].origin == vm::vec3d{1.5, -2, 300});
    }

    SECTION("leaves out the working directory")
    {
      const auto workDir = std::filesystem::path{"/tmp/work"};
      const auto messages = parseCompilerLog(
        "WARNING: Couldn't load map file \"" + workDir.string() + "/m.map\".\n", workDir);
      REQUIRE(messages.size() == 1);
      CHECK(messages[0].message == "Couldn't load map file \"m.map\".");
    }
  }

  SECTION("withoutLineNumbers")
  {
    CHECK(
      withoutLineNumbers("m.map[line 92]: brush has multiple face contents")
      == "m.map: brush has multiple face contents");
    CHECK(
      withoutLineNumbers("unable to find texture skip") == "unable to find texture skip");
  }

  SECTION("compile")
  {
    auto env = CliTestEnvironment{};
    const auto bytes = *readFile(fixturePath("compiled/compile_warnings.map"));
    auto options = CompileOptions{};
    options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_warnings");

    SECTION("runs qbsp with -keepprt and without -leaktest on the given bytes")
    {
      const auto result = compile(bytes, "my map", "rev", options);
      REQUIRE(result.runs.size() == 1);
      const auto& run = result.runs[0];
      CHECK(run.tool == "qbsp");
      CHECK(run.process.exitCode == 0);
      CHECK_FALSE(run.failed());
      CHECK(run.timeout == DefaultQbspTimeout);
      CHECK(run.messages.size() == 8);
      CHECK_THAT(run.messages[1].message, StartsWith("my map.map[line 92]: "));
      CHECK_FALSE(result.leaked);

      CHECK(env.readFile("tools/qbsp.args") == "-keepprt\nmy map.map\n");
      CHECK(env.readFile("tools/qbsp.input") == bytes);

      // the temporary directory is deleted
      const auto workDir = std::filesystem::path{env.readFile("tools/qbsp.cwd")};
      CHECK_FALSE(std::filesystem::exists(workDir));
    }

    SECTION("gives qbsp the given flags instead of -keepprt")
    {
      options.qbspFlags = std::vector<std::string>{"-nodetail", "-forceprt1"};
      compile(bytes, "m", "rev", options);
      CHECK(env.readFile("tools/qbsp.args") == "-nodetail\n-forceprt1\nm.map\n");
    }

    SECTION("reports a failed run")
    {
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_fatal");
      const auto result = compile(bytes, "m", "rev", options);
      CHECK(result.runs[0].process.exitCode == 1);
      CHECK(result.runs[0].failed());
      CHECK(result.runs[0].messages.size() == 2);
    }

    SECTION("uses the configured timeout unless one is given")
    {
      options.compilers.qbsp.path =
        setUpFakeCompiler(env, QbspTool, "compile_warnings", 10000);
      options.compilers.qbsp.timeout = 200ms;
      auto result = compile(bytes, "m", "rev", options);
      CHECK(result.runs[0].process.timedOut);
      CHECK(result.runs[0].timeout == 200ms);
      CHECK(result.runs[0].failed());

      options.compilers.qbsp.timeout = 10min;
      options.timeout = 100ms;
      result = compile(bytes, "m", "rev", options);
      CHECK(result.runs[0].process.timedOut);
      CHECK(result.runs[0].timeout == 100ms);
    }

    SECTION("keeps only the log of a failed run")
    {
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_fatal");
      options.artifacts = env.path("artifacts");
      // the artifacts of an earlier, successful run
      env.writeFile("artifacts/m.bsp", "bsp");
      env.writeFile("artifacts/m.prt", "prt");

      compile(bytes, "m", "abc123", options);
      CHECK(std::filesystem::is_regular_file(env.path("artifacts/m.log")));
      CHECK_FALSE(std::filesystem::exists(env.path("artifacts/m.bsp")));
      CHECK_FALSE(std::filesystem::exists(env.path("artifacts/m.prt")));
      CHECK(
        parseJsonLiteral(env.readFile("artifacts/m.bsp.tbcli.json"))
        == parseJsonLiteral(R"({"rev": "abc123", "leaked": false})"));
    }

    SECTION("keeps the artifacts and writes the sidecar")
    {
      options.artifacts = env.path("artifacts");
      // a stale pointfile of an earlier, leaking run
      env.writeFile("artifacts/m.pts", "0 0 0\n");

      compile(bytes, "m", "abc123", options);
      CHECK(std::filesystem::is_regular_file(env.path("artifacts/m.bsp")));
      CHECK(std::filesystem::is_regular_file(env.path("artifacts/m.prt")));
      CHECK(std::filesystem::is_regular_file(env.path("artifacts/m.log")));
      CHECK_FALSE(std::filesystem::exists(env.path("artifacts/m.pts")));
      CHECK(
        parseJsonLiteral(env.readFile("artifacts/m.bsp.tbcli.json"))
        == parseJsonLiteral(R"({"rev": "abc123", "leaked": false})"));
    }

    SECTION("reads the pointfile of a leaking map and skips vis and light")
    {
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_leak");
      options.compilers.vis.path = setUpFakeCompiler(env, VisTool, "compile_full");
      options.compilers.light.path = setUpFakeCompiler(env, LightTool, "compile_full");
      options.level = CompileLevel::Light;
      options.artifacts = env.path("artifacts");

      const auto result = compile(bytes, "m", "abc123", options);
      CHECK(result.leaked);
      CHECK(result.leakTrail.size() == 8);
      CHECK(result.leakTrail.front() == vm::vec3d{144, 0, 96});
      REQUIRE(result.runs.size() == 1);
      CHECK_FALSE(result.runs[0].failed());
      CHECK(result.skipped == std::vector<std::string>{"vis", "light"});
      CHECK_FALSE(std::filesystem::exists(env.path("tools/vis.args")));

      // the raw points are only in the artifacts
      CHECK(
        *readFile(env.path("artifacts/m.pts"))
        == *readFile(fixturePath("compiled/compile_leak/qbsp/compile_leak.pts")));
      CHECK(
        parseJsonLiteral(env.readFile("artifacts/m.bsp.tbcli.json"))
        == parseJsonLiteral(R"({"rev": "abc123", "leaked": true})"));
    }

    SECTION("skips vis and light if qbsp fails")
    {
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_fatal");
      options.compilers.vis.path = setUpFakeCompiler(env, VisTool, "compile_full");
      options.level = CompileLevel::Vis;

      const auto result = compile(bytes, "m", "rev", options);
      CHECK_FALSE(result.leaked);
      CHECK(result.runs.size() == 1);
      CHECK(result.skipped == std::vector<std::string>{"vis"});
    }

    SECTION("runs vis -fast and light on the bsp of a sealed map")
    {
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_full");
      options.compilers.vis.path = setUpFakeCompiler(env, VisTool, "compile_full");
      options.compilers.light.path = setUpFakeCompiler(env, LightTool, "compile_full");
      options.artifacts = env.path("artifacts");

      SECTION("=vis runs qbsp and vis")
      {
        options.level = CompileLevel::Vis;
        const auto result = compile(bytes, "m", "rev", options);
        REQUIRE(result.runs.size() == 2);
        CHECK(result.runs[1].tool == "vis");
        CHECK(result.runs[1].timeout == DefaultVisTimeout);
        CHECK(result.runs[1].messages.empty());
        CHECK(result.skipped.empty());
        CHECK(env.readFile("tools/vis.args") == "-fast\nm.bsp\n");
        CHECK_FALSE(std::filesystem::exists(env.path("tools/light.args")));
        CHECK(std::filesystem::is_regular_file(env.path("artifacts/m-vis.log")));
      }

      SECTION("=light runs qbsp, vis and light")
      {
        options.level = CompileLevel::Light;
        // the light log of an earlier run is replaced
        env.writeFile("artifacts/m-light.log", "stale");

        const auto result = compile(bytes, "m", "rev", options);
        REQUIRE(result.runs.size() == 3);
        CHECK(result.runs[0].tool == "qbsp");
        CHECK(result.runs[1].tool == "vis");
        CHECK(result.runs[2].tool == "light");
        CHECK(result.runs[2].timeout == DefaultLightTimeout);
        CHECK(env.readFile("tools/light.args") == "m.bsp\n");
        CHECK_FALSE(result.leaked);

        // light's log is <map>-light.log
        const auto& light = result.runs[2].messages;
        REQUIRE(light.size() == 2);
        CHECK(
          light[0].message
          == R"(entity at () (worldspawn) has empty key/value "wad" "")");
        CHECK(light[1].message == "unknown delay 7 on light at [144 0 96]");
        CHECK(light[1].classname == "light");
        CHECK(light[1].origin == vm::vec3d{144, 0, 96});

        CHECK(std::filesystem::is_regular_file(env.path("artifacts/m-vis.log")));
        CHECK(env.readFile("artifacts/m-light.log") != "stale");
      }

      SECTION("a failed vis keeps no bsp, but light still runs")
      {
        options.compilers.vis.path =
          setUpFakeCompiler(env, VisTool, "compile_full", 10000);
        options.compilers.vis.timeout = 200ms;
        options.level = CompileLevel::Light;

        const auto result = compile(bytes, "m", "rev", options);
        REQUIRE(result.runs.size() == 3);
        CHECK(result.runs[1].process.timedOut);
        CHECK(result.runs[1].timeout == 200ms);
        CHECK_FALSE(result.runs[2].failed());
        CHECK(result.skipped.empty());
        CHECK_FALSE(std::filesystem::exists(env.path("artifacts/m.bsp")));
        CHECK_FALSE(std::filesystem::exists(env.path("artifacts/m.prt")));
        CHECK(std::filesystem::is_regular_file(env.path("artifacts/m-light.log")));
      }
    }

    SECTION("throws compiler_missing")
    {
      const auto code = [&]() {
        try
        {
          compile(bytes, "m", "rev", options);
        }
        catch (const CliError& e)
        {
          return e.code();
        }
        return ErrorCode::InternalError;
      };

      options.compilers.qbsp.path = std::nullopt;
      CHECK(code() == ErrorCode::CompilerMissing);

      options.compilers.qbsp.path = env.path("tools/missing");
      CHECK(code() == ErrorCode::CompilerMissing);

      // vis and light are needed only by the levels that run them, and are looked up
      // before qbsp runs
      options.compilers.qbsp.path = setUpFakeCompiler(env, QbspTool, "compile_warnings");
      options.level = CompileLevel::Vis;
      CHECK(code() == ErrorCode::CompilerMissing);
      CHECK_FALSE(std::filesystem::exists(env.path("tools/qbsp.args")));

      options.compilers.vis.path = setUpFakeCompiler(env, VisTool, "compile_full");
      options.level = CompileLevel::Light;
      CHECK(code() == ErrorCode::CompilerMissing);
    }
  }
}

TEST_CASE("ToolRun")
{
  SECTION("failed")
  {
    CHECK_FALSE(ToolRun{"qbsp", {0, false}}.failed());
    CHECK(ToolRun{"qbsp", {1, false}}.failed());
    CHECK(ToolRun{"qbsp", {std::nullopt, true}}.failed());
  }

  SECTION("failureMessage")
  {
    CHECK(ToolRun{"qbsp", {1, false}}.failureMessage() == "qbsp exited with code 1");
    CHECK(
      ToolRun{"vis", {std::nullopt, true}, 1500ms}.failureMessage()
      == "vis did not finish within 1.5 seconds and was stopped");
  }
}

} // namespace tb::cli
