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

#include "cli/ParseUtils.h"

#include "vm/util.h"

#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::cli
{

TEST_CASE("ParseUtils")
{
  SECTION("parseBool")
  {
    CHECK(parseBool("true") == true);
    CHECK(parseBool("false") == false);

    const auto invalid = GENERATE(
      std::string_view{""},
      std::string_view{"True"},
      std::string_view{"1"},
      std::string_view{"yes"},
      std::string_view{"true "});
    CAPTURE(invalid);
    CHECK(parseBool(invalid) == std::nullopt);
  }

  SECTION("parseUnsigned")
  {
    CHECK(parseUnsigned("0") == 0);
    CHECK(parseUnsigned("42") == 42);
    CHECK(parseUnsigned("007") == 7);

    const auto invalid = GENERATE(
      std::string_view{""},
      std::string_view{"-1"},
      std::string_view{"+1"},
      std::string_view{" 1"},
      std::string_view{"1 "},
      std::string_view{"1.0"},
      std::string_view{"0x1"},
      std::string_view{"99999999999999999999999"});
    CAPTURE(invalid);
    CHECK(parseUnsigned(invalid) == std::nullopt);
  }

  SECTION("parseDecimal")
  {
    CHECK(parseDecimal("0") == 0.0);
    CHECK(parseDecimal("-16") == -16.0);
    CHECK(parseDecimal("+16") == 16.0);
    CHECK(parseDecimal("0.5") == 0.5);
    CHECK(parseDecimal(".5") == 0.5);
    CHECK(parseDecimal("2.") == 2.0);
    CHECK(parseDecimal("1e3") == 1000.0);
    CHECK(parseDecimal("1.5E-1") == 0.15);

    const auto invalid = GENERATE(
      std::string_view{""},
      std::string_view{"-"},
      std::string_view{"."},
      std::string_view{"1.2.3"},
      std::string_view{"1e"},
      std::string_view{"e3"},
      std::string_view{"1x"},
      std::string_view{" 1"},
      std::string_view{"nan"},
      std::string_view{"inf"});
    CAPTURE(invalid);
    CHECK(parseDecimal(invalid) == std::nullopt);
  }

  SECTION("parseNumberList")
  {
    using Numbers = std::vector<double>;
    CHECK(parseNumberList("1,2.5,-3") == Numbers{1.0, 2.5, -3.0});
    CHECK(parseNumberList("1 2.5  -3") == Numbers{1.0, 2.5, -3.0});
    CHECK(parseNumberList("1, 2,,3 ") == Numbers{1.0, 2.0, 3.0});
    CHECK(parseNumberList("") == Numbers{});
    CHECK(parseNumberList("1,x,3") == std::nullopt);
    CHECK(parseNumberList("1;2") == std::nullopt);
  }

  SECTION("axisNames")
  {
    CHECK(
      std::vector<std::string_view>(axisNames().begin(), axisNames().end())
      == std::vector<std::string_view>{"x", "y", "z"});
  }

  SECTION("parseAxis")
  {
    CHECK(parseAxis("x") == vm::axis::x);
    CHECK(parseAxis("z") == vm::axis::z);
    CHECK(parseAxis("X") == std::nullopt);
    CHECK(parseAxis("+z") == std::nullopt);
  }
}

} // namespace tb::cli
