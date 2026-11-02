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

#include "cli/Json.h"

#include <cstddef>
#include <ranges>
#include <vector>

namespace tb::cli
{

/** Which part of a long result list a command returns: `--limit` and `--offset`. */
struct Paging
{
  size_t limit = 200;
  size_t offset = 0;
};

/**
 * Sets `items`, `total` and `truncated` on `object`: the shape of every list in the
 * output that grows with the map.
 */
inline void setCapped(Json& object, Json items, const size_t total, const bool truncated)
{
  object.set("items", std::move(items));
  object.set("total", total);
  object.set("truncated", truncated);
}

/** Sets `items` (the first `max` elements), `total` and `truncated` on `object`. */
template <typename T, typename F>
void setCapped(
  Json& object, const std::vector<T>& elements, const size_t max, const F& toJson)
{
  auto items = Json::array();
  for (const auto& element : elements | std::views::take(max))
  {
    items.push(toJson(element));
  }
  setCapped(object, std::move(items), elements.size(), elements.size() > max);
}

/**
 * Sets `items` (the page of the given elements that the paging selects), `total` and
 * `truncated` on `object`.
 */
template <typename T, typename F>
void setPage(
  Json& object, const std::vector<T>& elements, const Paging& paging, const F& toJson)
{
  auto items = Json::array();
  for (const auto& element :
       elements | std::views::drop(paging.offset) | std::views::take(paging.limit))
  {
    items.push(toJson(element));
  }
  setCapped(
    object,
    std::move(items),
    elements.size(),
    elements.size() > paging.offset && elements.size() - paging.offset > paging.limit);
}

/** `{items, total, truncated}` for the page of the given elements. */
template <typename T, typename F>
Json page(const std::vector<T>& elements, const Paging& paging, const F& toJson)
{
  auto result = Json::object();
  setPage(result, elements, paging, toJson);
  return result;
}

/** `{items, total, truncated}` for the first `max` of the given elements. */
template <typename T, typename F>
Json capped(const std::vector<T>& elements, const size_t max, const F& toJson)
{
  auto result = Json::object();
  setCapped(result, elements, max, toJson);
  return result;
}

} // namespace tb::cli
