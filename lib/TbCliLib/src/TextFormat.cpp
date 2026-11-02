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

#include "cli/TextFormat.h"

#include "cli/Json.h"

#include <string_view>

namespace tb::cli
{
namespace
{

constexpr auto MaxInlineWidth = size_t{80};

std::string inlineText(const Json& json)
{
  if (json.isNull())
  {
    return "null";
  }
  if (json.isBool())
  {
    return json.asBool() ? "true" : "false";
  }
  if (json.isNumber())
  {
    return formatNumber(json.asNumber());
  }
  if (json.isString())
  {
    return json.asString();
  }

  auto result = std::string{};
  auto first = true;
  if (json.isArray())
  {
    result += "[";
    for (const auto& element : json.asArray())
    {
      result += first ? "" : ", ";
      result += inlineText(element);
      first = false;
    }
    result += "]";
  }
  else
  {
    result += "{";
    for (const auto& [key, value] : json.asObject())
    {
      result += first ? "" : ", ";
      result += key + ": " + inlineText(value);
      first = false;
    }
    result += "}";
  }
  return result;
}

bool isContainer(const Json& json)
{
  return json.isArray() || json.isObject();
}

void appendValue(
  std::string& out, std::string_view prefix, const Json& value, size_t indent);

void appendMembers(std::string& out, const Json& object, const size_t indent)
{
  for (const auto& [key, value] : object.asObject())
  {
    appendValue(out, std::string(indent, ' ') + key + ":", value, indent);
  }
}

void appendElements(std::string& out, const Json& array, const size_t indent)
{
  for (const auto& element : array.asArray())
  {
    appendValue(out, std::string(indent, ' ') + "-", element, indent);
  }
}

/**
 * Appends `prefix value` on one line if it fits, otherwise `prefix` followed by the
 * value's members or elements indented below it.
 */
void appendValue(
  std::string& out, const std::string_view prefix, const Json& value, const size_t indent)
{
  const auto text = inlineText(value);
  if (
    !isContainer(value) || value.size() == 0
    || prefix.size() + 1 + text.size() <= MaxInlineWidth)
  {
    out += prefix;
    out += " ";
    out += text;
    out += "\n";
    return;
  }

  out += prefix;
  out += "\n";
  if (value.isObject())
  {
    appendMembers(out, value, indent + 2);
  }
  else
  {
    appendElements(out, value, indent + 2);
  }
}

} // namespace

std::string toText(const Json& json)
{
  auto result = std::string{};
  if (json.isObject())
  {
    appendMembers(result, json, 0);
  }
  else if (json.isArray())
  {
    appendElements(result, json, 0);
  }
  else
  {
    result = inlineText(json) + "\n";
  }
  return result;
}

} // namespace tb::cli
