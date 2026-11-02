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

#include <fmt/format.h>

namespace tb::cli
{

Json toJson(const vm::vec3d& v)
{
  return Json{Json::Array{v.x(), v.y(), v.z()}};
}

Json toJson(const vm::bbox3d& bounds)
{
  auto result = Json::object();
  result.set("min", toJson(bounds.min));
  result.set("max", toJson(bounds.max));
  return result;
}

std::string formatVec3(const vm::vec3d& v)
{
  return fmt::format(
    "{} {} {}", formatNumber(v.x()), formatNumber(v.y()), formatNumber(v.z()));
}

vm::vec3d vec3FromJson(const Json& json)
{
  return {json[0].asNumber(), json[1].asNumber(), json[2].asNumber()};
}

vm::bbox3d bboxFromJson(const Json& json)
{
  return {vec3FromJson(json["min"]), vec3FromJson(json["max"])};
}

} // namespace tb::cli
