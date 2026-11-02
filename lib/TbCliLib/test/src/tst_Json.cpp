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

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("Json")
{
  SECTION("dump")
  {
    auto object = Json::object();
    object.set("b", true);
    object.set("a", Json{Json::Array{1, 2.5, nullptr, "x"}});
    object.set("s", "quote \" backslash \\ newline \n tab \t");
    const auto expected =
      R"({"b":true,"a":[1,2.5,null,"x"],"s":"quote \" backslash \\ newline \n tab \t"})";
    CHECK(object.dump() == expected);
  }

  SECTION("set")
  {
    auto object = Json::object();
    object.set("a", 1);
    object.set("b", 2);
    object.set("a", 3);
    CHECK(object.dump() == R"({"a":3,"b":2})");
  }

  SECTION("find and operator[]")
  {
    auto object = Json::object();
    object.set("a", Json{Json::Array{1, 2}});
    REQUIRE(object.find("a") != nullptr);
    CHECK(object.find("b") == nullptr);
    CHECK(object["a"][1] == Json{2});
    CHECK(object["a"][2].isNull());
    CHECK(object["b"].isNull());
    CHECK(Json{1}["a"].isNull());
  }

  SECTION("size")
  {
    CHECK(Json{1}.size() == 0);
    CHECK(Json{Json::Array{1, 2}}.size() == 2);
    CHECK(Json::object().set("a", 1).size() == 1);
  }
}

TEST_CASE("Json functions")
{
  SECTION("formatNumber")
  {
    CHECK(formatNumber(0.0) == "0");
    CHECK(formatNumber(-0.0) == "0");
    CHECK(formatNumber(64.0) == "64");
    CHECK(formatNumber(-16.0) == "-16");
    CHECK(formatNumber(108.1234) == "108.123");
    CHECK(formatNumber(0.1) == "0.1");
    CHECK(formatNumber(-0.25) == "-0.25");
    CHECK(formatNumber(1.9996) == "2");
    CHECK(formatNumber(-0.0004) == "0");
    CHECK(formatNumber(1e20) == "100000000000000000000");
  }

  SECTION("parseJson")
  {
    const auto parsed =
      parseJson(" {\"a\": [1, -2.5e1, true, false, null], \"b\": \"\\u00e9\\n\"} ");
    REQUIRE(parsed.is_success());
    const auto& json = parsed.value();
    CHECK(json["a"].size() == 5);
    CHECK(json["a"][0] == Json{1});
    CHECK(json["a"][1] == Json{-25});
    CHECK(json["a"][2] == Json{true});
    CHECK(json["a"][4].isNull());
    CHECK(json["b"] == Json{"\xc3\xa9\n"});
    CHECK(json["missing"].isNull());

    CHECK(parseJson(json.dump()).value() == json);

    const auto error = parseJson("{\n\"a\": 1,\n}");
    REQUIRE(error.is_error());
    CHECK(std::get<JsonParseError>(error.error()).line == 3);

    CHECK(parseJson("").is_error());
    CHECK(parseJson("[1,]").is_error());
    CHECK(parseJson("{} x").is_error());
    CHECK(parseJson("\"unterminated").is_error());
  }
}

} // namespace tb::cli
