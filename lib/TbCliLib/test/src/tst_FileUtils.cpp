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
#include "cli/FileUtils.h"

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::cli
{
using namespace Catch::Matchers;

namespace
{

std::vector<std::string> fileNames(const std::filesystem::path& dir)
{
  auto result = std::vector<std::string>{};
  for (const auto& entry : std::filesystem::directory_iterator{dir})
  {
    result.push_back(entry.path().filename().string());
  }
  return result;
}

} // namespace

TEST_CASE("FileUtils")
{
  auto env = CliTestEnvironment{};

  SECTION("readFile")
  {
    const auto bytes = std::string{"a\r\nb\n\0c", 7};
    const auto path = env.writeFile("file.bin", bytes);
    CHECK(readFile(path) == bytes);
    CHECK(readFile(env.path("missing")) == std::nullopt);
  }

  SECTION("writeFile")
  {
    const auto bytes = std::string{"a\r\nb\n\0c", 7};
    const auto path = env.writeFile("file.bin", "old contents");
    CHECK(writeFile(path, bytes) == std::nullopt);
    CHECK(readFile(path) == bytes);
    CHECK(writeFile(env.path("missing/file.bin"), bytes).has_value());
  }

  SECTION("writeFileAtomically")
  {
    const auto path = env.writeFile("file.map", "old");

    SECTION("replaces the file")
    {
      CHECK(writeFileAtomically(path, "new\r\ncontents") == std::nullopt);
      // read in binary mode, unlike CliTestEnvironment::readFile
      CHECK(readFile(path) == "new\r\ncontents");
      CHECK(fileNames(env.path("")) == std::vector<std::string>{"file.map"});
    }

    SECTION("creates a missing file")
    {
      CHECK(writeFileAtomically(env.path("other.map"), "new") == std::nullopt);
      CHECK(env.readFile("other.map") == "new");
    }

    SECTION("a write that does not read back as written leaves the file intact")
    {
      const auto readBadly = [](const std::filesystem::path&) {
        return std::optional<std::string>{"garbled"};
      };
      CHECK(writeFileAtomically(path, "new", readBadly) != std::nullopt);
      CHECK(env.readFile("file.map") == "old");
      CHECK(fileNames(env.path("")) == std::vector<std::string>{"file.map"});
    }

    SECTION("a file in a missing directory cannot be written")
    {
      CHECK(writeFileAtomically(env.path("missing/file.map"), "new") != std::nullopt);
    }

    SECTION("a failed rename leaves the target intact and removes the temporary file")
    {
      std::filesystem::create_directories(env.path("dir"));
      CHECK(writeFileAtomically(env.path("dir"), "new") != std::nullopt);
      CHECK(std::filesystem::is_directory(env.path("dir")));
      CHECK_THAT(
        fileNames(env.path("")),
        UnorderedEquals(std::vector<std::string>{"dir", "file.map"}));
    }
  }
}

} // namespace tb::cli
