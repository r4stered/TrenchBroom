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


#include "cli/GeometryJson.h"
#include "cli/Json.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <catch2/catch_test_macros.hpp>

namespace tb::cli
{

TEST_CASE("GeometryJson")
{
  SECTION("toJson")
  {
    CHECK(toJson(vm::vec3d{1, -2.5, 3}) == Json{Json::Array{1, -2.5, 3}});
    CHECK(
      toJson(vm::bbox3d{{0, 1, 2}, {3, 4, 5}}).dump()
      == R"({"min":[0,1,2],"max":[3,4,5]})");
  }

  SECTION("formatVec3")
  {
    CHECK(formatVec3(vm::vec3d{1, -2.5, 1.0 / 3.0}) == "1 -2.5 0.333");
  }

  SECTION("vec3FromJson")
  {
    CHECK(vec3FromJson(Json{Json::Array{1, -2.5, 3}}) == vm::vec3d{1, -2.5, 3});
  }

  SECTION("bboxFromJson")
  {
    const auto bounds = vm::bbox3d{{0, 1, 2}, {3, 4, 5}};
    CHECK(bboxFromJson(toJson(bounds)) == bounds);
  }
}

} // namespace tb::cli
