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

#include "cli/NodePath.h"

#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>

namespace tb::cli
{
namespace
{

/** The kind of a node as named in its path segment. */
std::string_view kindName(const mdl::Node& node)
{
  return node.accept(
    kdl::overload(
      // the world is never a child, so it never needs a segment
      [](const mdl::WorldNode&) { return WorldPath; },
      [](const mdl::LayerNode&) { return std::string_view{"layer"}; },
      [](const mdl::GroupNode&) { return std::string_view{"group"}; },
      [](const mdl::EntityNode&) { return std::string_view{"entity"}; },
      [](const mdl::BrushNode&) { return std::string_view{"brush"}; },
      [](const mdl::PatchNode&) { return std::string_view{"patch"}; }));
}

std::string segment(const mdl::Node& node, const mdl::Node& parent)
{
  const auto kind = kindName(node);
  const auto& siblings = parent.children();
  const auto end = std::ranges::find(siblings, &node);
  const auto index = std::count_if(siblings.begin(), end, [&](const auto* sibling) {
    return kindName(*sibling) == kind;
  });
  return fmt::format("{}:{}", kind, index);
}

} // namespace

std::string nodePath(const mdl::Node& node)
{
  const auto* parent = node.parent();
  if (!parent)
  {
    return std::string{WorldPath};
  }

  const auto own = segment(node, *parent);
  return parent->parent() ? nodePath(*parent) + "/" + own : own;
}

} // namespace tb::cli
