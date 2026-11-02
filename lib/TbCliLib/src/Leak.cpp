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

#include "cli/Leak.h"

#include "cli/Compile.h"
#include "cli/ParseUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"

#include "kd/ranges/to.h"

#include "vm/bbox.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace tb::cli
{
namespace
{

/** How far apart two points of a trail may be and still be the same point. */
constexpr auto PointEpsilon = 0.001;

/**
 * How far the directions of two segments may differ, as the sine of the angle between
 * them, and still be one straight line.
 */
constexpr auto DirectionEpsilon = 1e-6;

/** The precision of the distance between a brush and the exit point of a trail. */
constexpr auto DistancePrecision = 0.001;

/** Whether `b` lies on the straight line from `a` to `c`, between the two. */
bool isStraight(const vm::vec3d& a, const vm::vec3d& b, const vm::vec3d& c)
{
  const auto u = b - a;
  const auto v = c - b;
  return vm::dot(u, v) > 0.0
         && vm::length(vm::cross(u, v))
              <= DirectionEpsilon * vm::length(u) * vm::length(v);
}

/**
 * The distance between a box and a point, rounded so that brushes at the same
 * distance stay in file order.
 */
double distance(const vm::bbox3d& bounds, const vm::vec3d& point)
{
  const auto exact = vm::distance(bounds.constrain(point), point);
  return std::round(exact / DistancePrecision) * DistancePrecision;
}

/**
 * Whether the brush can seal the map: qbsp seals the map only with the brushes of
 * worldspawn and of the entities it merges into worldspawn or treats as detail.
 */
bool isSealing(const mdl::BrushNode& brushNode)
{
  const auto* entityNode = brushNode.entity();
  if (!entityNode)
  {
    return false;
  }
  const auto& classname = entityNode->entity().classname();
  return classname == mdl::EntityPropertyValues::WorldspawnClassname
         || classname == mdl::EntityPropertyValues::GroupClassname
         || classname == "func_detail";
}

/** The brushes under the given node that can seal the map, in file order. */
void collectSealingBrushes(mdl::Node& node, std::vector<mdl::BrushNode*>& brushes)
{
  if (
    auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node);
    brushNode && isSealing(*brushNode))
  {
    brushes.push_back(brushNode);
  }
  for (auto* child : node.children())
  {
    collectSealingBrushes(*child, brushes);
  }
}

/** The point where the trail leaves the given bounds; see brushesNearExit. */
const vm::vec3d& exitPoint(const std::vector<vm::vec3d>& trail, const vm::bbox3d& bounds)
{
  const auto outside = std::ranges::find_if(
    trail, [&](const auto& point) { return !bounds.contains(point); });
  if (outside == trail.end())
  {
    return trail.back();
  }
  return outside == trail.begin() ? *outside : *std::prev(outside);
}

} // namespace

std::optional<std::vector<vm::vec3d>> parsePointFile(const std::string_view text)
{
  auto result = std::vector<vm::vec3d>{};
  for (const auto range : std::views::split(text, '\n'))
  {
    auto line = std::string_view{range.begin(), range.end()};
    if (line.find_first_not_of(" \t\r") == std::string_view::npos)
    {
      continue;
    }

    const auto numbers =
      parseNumberList(line.substr(0, line.find_last_not_of(" \t\r") + 1));
    if (!numbers || numbers->size() != 3)
    {
      return std::nullopt;
    }
    result.emplace_back((*numbers)[0], (*numbers)[1], (*numbers)[2]);
  }
  return result;
}

std::vector<vm::vec3d> simplifyTrail(const std::vector<vm::vec3d>& trail)
{
  auto result = std::vector<vm::vec3d>{};
  for (const auto& point : trail)
  {
    if (!result.empty() && vm::is_equal(result.back(), point, PointEpsilon))
    {
      continue;
    }
    if (result.size() >= 2 && isStraight(result[result.size() - 2], result.back(), point))
    {
      result.back() = point;
      continue;
    }
    result.push_back(point);
  }
  return result;
}

std::vector<mdl::BrushNode*> brushesNearExit(
  mdl::Node& root, const std::vector<vm::vec3d>& trail, const size_t count)
{
  auto brushes = std::vector<mdl::BrushNode*>{};
  collectSealingBrushes(root, brushes);
  if (trail.empty() || brushes.empty())
  {
    return {};
  }

  auto bounds = brushes.front()->logicalBounds();
  for (const auto* brush : brushes)
  {
    bounds = vm::merge(bounds, brush->logicalBounds());
  }
  const auto& exit = exitPoint(trail, bounds);

  auto byDistance = std::vector<std::pair<double, mdl::BrushNode*>>{};
  for (auto* brush : brushes)
  {
    byDistance.emplace_back(distance(brush->logicalBounds(), exit), brush);
  }
  std::ranges::stable_sort(byDistance, {}, &std::pair<double, mdl::BrushNode*>::first);

  return byDistance | std::views::take(count)
         | std::views::transform(&std::pair<double, mdl::BrushNode*>::second)
         | kdl::ranges::to<std::vector<mdl::BrushNode*>>();
}

bool isLeakOccupant(const CompilerMessage& message)
{
  return message.message.starts_with("Reached occupant ");
}

} // namespace tb::cli
