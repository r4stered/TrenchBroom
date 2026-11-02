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

#pragma once

#include "kd/result.h"

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tb::cli
{

struct JsonMember;

/**
 * A JSON value. Objects keep their members in insertion order so that output is
 * stable and reads in the order the schema documents it.
 *
 * Numbers are doubles. When written, they are rounded to 3 decimals and integral
 * values are written without a fractional part.
 *
 * TrenchBroom's only JSON support outside of tbcli is Qt's, which TbCliLib must not
 * depend on, hence this small implementation.
 */
class Json
{
public:
  using Array = std::vector<Json>;
  using Object = std::vector<JsonMember>;

private:
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_value;

public:
  Json();
  Json(std::nullptr_t);
  Json(bool b);
  Json(double d);
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Json(const T i)
    : Json{static_cast<double>(i)}
  {
  }
  Json(const char* s);
  Json(std::string_view s);
  Json(std::string s);
  Json(Array a);
  Json(Object o);

  static Json object();
  static Json array();

  bool isNull() const;
  bool isBool() const;
  bool isNumber() const;
  bool isString() const;
  bool isArray() const;
  bool isObject() const;

  bool asBool() const;
  double asNumber() const;
  const std::string& asString() const;
  const Array& asArray() const;
  const Object& asObject() const;

  /** Object access: the member with the given key, or nullptr. */
  const Json* find(std::string_view key) const;

  /** Object access: the member with the given key, or null if there is none. */
  const Json& operator[](std::string_view key) const;

  /** Array access: the element at the given index, or null if out of range. */
  const Json& operator[](size_t index) const;

  /** Number of array elements or object members, 0 otherwise. */
  size_t size() const;

  /** Object: replaces the member with the given key or appends it. */
  Json& set(std::string key, Json value);

  /** Array: appends an element. */
  Json& push(Json value);

  /** Writes this value as compact JSON. */
  std::string dump() const;

  friend bool operator==(const Json& lhs, const Json& rhs);
};

struct JsonMember
{
  std::string key;
  Json value;

  friend bool operator==(const JsonMember& lhs, const JsonMember& rhs) = default;
};

/** Formats a number as JSON output does: rounded to 3 decimals, integers as integers. */
std::string formatNumber(double d);

struct JsonParseError
{
  std::string msg;
  size_t line = 0;
};

/** Parses a strict JSON document. */
kdl::result<Json, JsonParseError> parseJson(std::string_view str);

} // namespace tb::cli
