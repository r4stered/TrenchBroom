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

#include "base/NotifierConnection.h"
#include "cli/Json.h"
#include "cli/NodePath.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tb::mdl
{
class BrushNode;
class Map;
class Node;
class WorldNode;
} // namespace tb::mdl

namespace tb::cli
{

/** Whether the given text is a node handle (`n:...`) rather than a node path. */
bool isNodeHandle(std::string_view text);

/**
 * The node handles of a live session: `n:<epoch>.<k>`, a name for a node object that
 * stays the same while the mapper edits other nodes, unlike its path. A handle is given
 * out the first time a command reports the node, and is never written to the map file.
 *
 * Undoing the deletion of a node brings back the same node object, so its handle names
 * it again. Duplicating or pasting a node creates a new node with a handle of its own.
 * Handles carry the session's epoch, so that a handle from an earlier session of the
 * same map gives `stale_session` rather than naming another node.
 *
 * Nodes are told apart by their serial numbers, not their addresses, since a node
 * created later may take the address of one that the undo history let go of.
 *
 * A table belongs to the map as it was loaded: loading or reloading the map starts a
 * new session, with a new epoch and a new table.
 */
class NodeHandleTable
{
private:
  mdl::Map& m_map;
  std::string m_epoch;
  /** The serial number of the node with the handle `k` at index `k - 1`. */
  std::vector<uint64_t> m_serialNumbers;
  std::unordered_map<uint64_t, size_t> m_handles;

  /** The nodes in the map by serial number, rebuilt when nodes were added or removed. */
  std::unordered_map<uint64_t, mdl::Node*> m_nodes;
  bool m_indexValid = false;
  NotifierConnection m_notifierConnection;

public:
  /** A table for the map's session with the given epoch. */
  NodeHandleTable(mdl::Map& map, std::string epoch);

  // observes the map through `this`
  NodeHandleTable(const NodeHandleTable&) = delete;
  NodeHandleTable& operator=(const NodeHandleTable&) = delete;

  /** The handle of the given node, given out now if the node has none yet. */
  std::string handle(const mdl::Node& node);

  /** The handle of a face: the handle of its brush followed by `/face:<index>`. */
  std::string faceHandle(const mdl::BrushNode& brushNode, size_t faceIndex);

  /**
   * The node that the given handle names, or the face if the handle is followed by
   * `/face:<index>`.
   *
   * @throw CliError `invalid_path` if the handle is malformed or a face follows a node
   * that is not a brush, `stale_session` if it is from another session, `node_not_found`
   * if this session never gave it out or the brush has no such face, and `gone` if its
   * node is no longer in the map
   */
  NodeOrFace resolve(std::string_view handle);

private:
  mdl::Node* findNode(uint64_t serialNumber);
};

/**
 * Finds what a node reference addresses: a node path, or in a live session (given its
 * handles) also a node handle.
 *
 * @throw CliError as resolveNodePath does for paths, and as NodeHandleTable::resolve
 * does for handles
 */
NodeOrFace resolveNodeReference(
  mdl::WorldNode& worldNode, std::string_view reference, NodeHandleTable* handles);

/**
 * The key that holds the handles next to a key that holds node paths: `handle` next to
 * `path`, `<key>_handle` next to any other key that holds one path, and `<key>_handles`
 * next to one that holds a list or an object of paths.
 */
std::string handleKey(std::string_view key, bool isList = false);

/**
 * Sets `key` to the given node's path and, in a live session (given its handles), the
 * handle key (see handleKey) to its handle.
 */
void setNodePath(
  Json& object, std::string_view key, const mdl::Node& node, NodeHandleTable* handles);

/** As above, with the node's path computed already. */
void setNodePath(
  Json& object,
  std::string_view key,
  std::string path,
  const mdl::Node& node,
  NodeHandleTable* handles);

/** Sets `key` to the path of a brush face and in a live session its handle. */
void setFacePath(
  Json& object,
  std::string_view key,
  const mdl::BrushNode& brushNode,
  size_t faceIndex,
  NodeHandleTable* handles);

/**
 * A list of node paths for the output and, in a live session, the list of their handles
 * that goes next to it.
 */
class NodePathList
{
private:
  NodeHandleTable* m_handleTable;
  Json m_paths = Json::array();
  Json m_handles = Json::array();

public:
  /** A list with handles if the handles of a live session are given. */
  explicit NodePathList(NodeHandleTable* handles);

  /** Appends the given path of the given node. */
  void push(std::string path, const mdl::Node& node);

  /** Appends the path of the given node. */
  void push(const mdl::Node& node);

  size_t size() const;

  /** The paths. */
  const Json& paths() const;

  /** The handles, which are only filled in a live session. */
  const Json& handles() const;

  /**
   * Sets `key` to the paths and, in a live session, the handle key (see handleKey) to
   * the handles.
   */
  void setOn(Json& object, std::string_view key) &&;
};

} // namespace tb::cli
