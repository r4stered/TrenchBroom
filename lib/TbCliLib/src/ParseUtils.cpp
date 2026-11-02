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

#include "kd/string_utils.h"

#include <algorithm>

namespace tb::cli
{
namespace
{

bool isDigit(const char c)
{
  return c >= '0' && c <= '9';
}

/** Consumes a non-empty run of digits from the front of `str`. */
bool consumeDigits(std::string_view& str)
{
  const auto count = size_t(std::ranges::find_if_not(str, isDigit) - str.begin());
  str.remove_prefix(count);
  return count > 0;
}

/** Whether the whole string is a decimal number, with an optional sign and exponent. */
bool isDecimal(std::string_view str)
{
  if (!str.empty() && (str.front() == '-' || str.front() == '+'))
  {
    str.remove_prefix(1);
  }

  const auto hasIntegerPart = consumeDigits(str);
  auto hasFractionalPart = false;
  if (!str.empty() && str.front() == '.')
  {
    str.remove_prefix(1);
    hasFractionalPart = consumeDigits(str);
  }
  if (!hasIntegerPart && !hasFractionalPart)
  {
    return false;
  }

  if (!str.empty() && (str.front() == 'e' || str.front() == 'E'))
  {
    str.remove_prefix(1);
    if (!str.empty() && (str.front() == '-' || str.front() == '+'))
    {
      str.remove_prefix(1);
    }
    if (!consumeDigits(str))
    {
      return false;
    }
  }
  return str.empty();
}

} // namespace

std::optional<size_t> parseUnsigned(const std::string_view str)
{
  return !str.empty() && std::ranges::all_of(str, isDigit) ? kdl::str_to_size(str)
                                                           : std::nullopt;
}

std::optional<double> parseDecimal(const std::string_view str)
{
  // kdl::str_to_double accepts a valid prefix, so the whole string is checked first
  return isDecimal(str) ? kdl::str_to_double(str) : std::nullopt;
}

} // namespace tb::cli
