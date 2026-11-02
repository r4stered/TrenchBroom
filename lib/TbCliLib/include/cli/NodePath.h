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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tb::mdl
{
class BrushNode;
class Node;
class WorldNode;
} // namespace tb::mdl

namespace tb::cli
{

/** The path of the world node, which is also the worldspawn entity. */
inline constexpr auto WorldPath = std::string_view{"world"};

/** What a node path segment names. */
enum class NodeKind
{
  World,
  Layer,
  Group,
  Entity,
  Brush,
  Patch,
  Face,
};

/** The kind of the given node; never Face. */
NodeKind kindOf(const mdl::Node& node);

/** Whether a node of the kind `parent` can contain a node of the kind `kind`. */
bool canContain(NodeKind parent, NodeKind kind);

/**
 * The node path of the given node: `layer:<n>/group:<n>/entity:<n>/brush:<n>`, where
 * each index counts the preceding siblings of the same kind in file order. `layer:0`
 * is the default layer. The world node's path is `world`.
 *
 * Paths are positional and only valid for the rev they were computed from.
 */
std::string nodePath(const mdl::Node& node);

/**
 * The paths of the world and every node below it, as nodePath returns them, computed in
 * one pass over the map.
 */
std::unordered_map<const mdl::Node*, std::string> nodePaths(
  const mdl::WorldNode& worldNode);

/** The path of a brush face: the brush's path followed by `face:<index>`. */
std::string facePath(const mdl::BrushNode& brushNode, size_t faceIndex);

/** A node, or a face of a brush node: what a node path addresses or a query matches. */
struct NodeOrFace
{
  mdl::Node* node = nullptr;
  /** Set if the path addresses a face of `node`, which is then a brush node. */
  std::optional<size_t> faceIndex;
};

/**
 * Finds what the given node path addresses in the given world.
 *
 * Besides the paths that nodePath and facePath return, a layer or group segment may
 * name the node by its persistent id instead of its index, as in `layer:id:<_tb_id>` or
 * `group:id:<_tb_id>`. Since persistent ids are unique in a map, a path may also start
 * with a group segment that names a group by id, wherever the group is.
 *
 * @throw CliError `invalid_path` if the path is malformed or names a node kind that
 * cannot appear where it does, `node_not_found` if the path is well-formed but addresses
 * nothing
 */
NodeOrFace resolveNodePath(mdl::WorldNode& worldNode, std::string_view path);

} // namespace tb::cli
