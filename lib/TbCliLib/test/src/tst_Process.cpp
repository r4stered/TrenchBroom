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
#include "cli/Process.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{
using namespace std::chrono_literals;

TEST_CASE("Process")
{
  SECTION("runProcess")
  {
    auto env = CliTestEnvironment{};
    env.copyFixture("compiled/compile_fatal.map", "work dir/compile_fatal.map");
    const auto workDir = env.path("work dir");

    SECTION("runs the executable in the working directory and returns its exit code")
    {
      const auto qbsp = setUpFakeCompiler(env, QbspTool, "compile_fatal");
      const auto result = runProcess(qbsp, {"compile_fatal.map"}, workDir, 10s);
      CHECK(result.exitCode == 1);
      CHECK_FALSE(result.timedOut);
      CHECK(std::filesystem::path{env.readFile("tools/qbsp.cwd")} == workDir);
      CHECK(std::filesystem::is_regular_file(workDir / "compile_fatal.log"));
    }

    SECTION("returns exit code 0")
    {
      const auto qbsp = setUpFakeCompiler(env, QbspTool, "compile_warnings");
      const auto result = runProcess(qbsp, {"compile_fatal.map"}, workDir, 10s);
      CHECK(result.exitCode == 0);
      CHECK_FALSE(result.timedOut);
    }

    SECTION("passes arguments unchanged")
    {
      const auto qbsp = setUpFakeCompiler(env, QbspTool, "compile_warnings");
      const auto arguments = std::vector<std::string>{
        "-keepprt",
        "two words",
        R"(a "quoted" word)",
        R"(trailing\)",
        "compile_fatal.map"};
      runProcess(qbsp, arguments, workDir, 10s);

      auto expected = std::string{};
      for (const auto& argument : arguments)
      {
        expected += argument + "\n";
      }
      CHECK(env.readFile("tools/qbsp.args") == expected);
    }

    SECTION("stops a process that runs longer than the timeout")
    {
      const auto qbsp = setUpFakeCompiler(env, QbspTool, "compile_warnings", 10000);
      const auto start = std::chrono::steady_clock::now();
      const auto result = runProcess(qbsp, {"compile_fatal.map"}, workDir, 200ms);
      CHECK(result.timedOut);
      CHECK(result.exitCode == std::nullopt);
      CHECK(std::chrono::steady_clock::now() - start < 5s);
    }

    SECTION("throws if the executable cannot be started")
    {
      CHECK_THROWS_AS(runProcess(env.path("tools/missing"), {}, workDir, 10s), CliError);
    }
  }

  SECTION("isBareName")
  {
    CHECK(isBareName("qbsp"));
    CHECK(isBareName("qbsp.exe"));
    CHECK_FALSE(isBareName("tools/qbsp"));
    CHECK_FALSE(isBareName("./qbsp"));
    CHECK_FALSE(isBareName(std::filesystem::current_path() / "qbsp"));
  }

  SECTION("findExecutable")
  {
    auto env = CliTestEnvironment{};
    const auto qbsp = setUpFakeCompiler(env, QbspTool, "compile_warnings");

    CHECK(findExecutable(qbsp) == qbsp);
    CHECK(findExecutable(env.path("tools/missing")) == std::nullopt);

#ifdef _WIN32
    // .exe is appended to a name without an extension
    CHECK(findExecutable(env.path("tools/qbsp")) == qbsp);
    // a bare name is looked up in PATH
    CHECK(findExecutable("cmd").has_value());
#else
    CHECK(findExecutable("sh").has_value());
#endif
    CHECK(findExecutable("tbcli-no-such-executable") == std::nullopt);
  }
}

} // namespace tb::cli
