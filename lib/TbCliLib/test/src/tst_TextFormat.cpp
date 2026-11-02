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
#include "cli/Json.h"
#include "cli/TextFormat.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("TextFormat")
{
  SECTION("toText")
  {
    SECTION("scalars")
    {
      CHECK(toText(Json{}) == "null\n");
      CHECK(toText(Json{1.23456}) == "1.235\n");
      CHECK(toText(Json{"text"}) == "text\n");
    }

    SECTION("short containers are written on one line")
    {
      auto inner = Json::object();
      inner.set("a", 1);
      inner.set("b", Json{Json::Array{true, false}});

      auto json = Json::object();
      json.set("name", "x");
      json.set("inner", inner);
      json.set("empty", Json::array());
      CHECK(toText(json) == "name: x\ninner: {a: 1, b: [true, false]}\nempty: []\n");
    }

    SECTION("long containers are expanded and indented")
    {
      const auto longString = std::string(60, 'x');

      auto element = Json::object();
      element.set("value", longString);

      auto inner = Json::object();
      inner.set("first", longString);
      inner.set("list", Json{Json::Array{element, 2}});

      auto json = Json::object();
      json.set("inner", inner);
      CHECK(
        toText(json)
        == "inner:\n"
           "  first: "
             + longString
             + "\n"
               "  list:\n"
               "    - {value: "
             + longString
             + "}\n"
               "    - 2\n");
    }
  }
}

} // namespace tb::cli
