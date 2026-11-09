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

#include "cli/NodeHandles.h"

#include "cli/CliError.h"
#include "cli/ParseUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/Map.h"
#include "mdl/WorldNode.h"

#include <fmt/format.h>

#include <optional>
#include <utility>

namespace tb::cli
{
namespace
{

constexpr auto HandlePrefix = std::string_view{"n:"};
constexpr auto FaceSuffix = std::string_view{"/face:"};

CliError invalidHandle(const std::string_view handle, const std::string_view reason)
{
  return CliError{
    ErrorCode::InvalidPath,
    fmt::format("invalid node handle '{}': {}", handle, reason),
    std::string{handle}};
}

/** A handle split into its parts: `n:<epoch>.<k>` and an optional `/face:<index>`. */
struct ParsedHandle
{
  std::string_view epoch;
  size_t k = 0;
  std::optional<size_t> faceIndex;
};

ParsedHandle parseHandle(const std::string_view handle)
{
  constexpr auto expected =
    "a handle is n:<epoch>.<k>, optionally followed by "
    "/face:<index>";

  auto rest = handle.substr(HandlePrefix.size());
  auto result = ParsedHandle{};
  if (const auto slash = rest.find('/'); slash != std::string_view::npos)
  {
    const auto face = rest.substr(slash);
    if (!face.starts_with(FaceSuffix))
    {
      throw invalidHandle(handle, expected);
    }
    const auto faceIndex = parseUnsigned(face.substr(FaceSuffix.size()));
    if (!faceIndex)
    {
      throw invalidHandle(handle, expected);
    }
    result.faceIndex = *faceIndex;
    rest = rest.substr(0, slash);
  }

  const auto dot = rest.find('.');
  if (dot == std::string_view::npos || dot == 0)
  {
    throw invalidHandle(handle, expected);
  }
  const auto k = parseUnsigned(rest.substr(dot + 1));
  if (!k || *k == 0)
  {
    throw invalidHandle(handle, expected);
  }
  result.epoch = rest.substr(0, dot);
  result.k = *k;
  return result;
}

void indexNodes(mdl::Node& node, std::unordered_map<uint64_t, mdl::Node*>& nodes)
{
  nodes.emplace(node.serialNumber(), &node);
  for (auto* child : node.children())
  {
    indexNodes(*child, nodes);
  }
}

} // namespace

bool isNodeHandle(const std::string_view text)
{
  return text.starts_with(HandlePrefix);
}

NodeHandleTable::NodeHandleTable(mdl::Map& map, std::string epoch)
  : m_map{map}
  , m_epoch{std::move(epoch)}
{
  const auto invalidateIndex = [&](const std::vector<mdl::Node*>&) {
    m_indexValid = false;
  };
  m_notifierConnection += m_map.nodesWereAddedNotifier.connect(invalidateIndex);
  m_notifierConnection += m_map.nodesWereRemovedNotifier.connect(invalidateIndex);
}

std::string NodeHandleTable::handle(const mdl::Node& node)
{
  const auto [it, inserted] =
    m_handles.try_emplace(node.serialNumber(), m_serialNumbers.size() + 1);
  if (inserted)
  {
    m_serialNumbers.push_back(node.serialNumber());
  }
  return fmt::format("{}{}.{}", HandlePrefix, m_epoch, it->second);
}

std::string NodeHandleTable::faceHandle(
  const mdl::BrushNode& brushNode, const size_t faceIndex)
{
  return fmt::format("{}{}{}", handle(brushNode), FaceSuffix, faceIndex);
}

NodeOrFace NodeHandleTable::resolve(const std::string_view handle)
{
  if (!isNodeHandle(handle))
  {
    throw invalidHandle(handle, "a handle starts with n:");
  }

  const auto parsed = parseHandle(handle);
  if (parsed.epoch != m_epoch)
  {
    throw CliError{
      ErrorCode::StaleSession,
      fmt::format(
        "the node handle '{}' is from an earlier session of this map, which was "
        "reopened or reloaded since; read the map again",
        handle),
      std::string{handle}};
  }
  if (parsed.k > m_serialNumbers.size())
  {
    throw CliError{
      ErrorCode::NodeNotFound,
      fmt::format("no node has the handle '{}' in this session", handle),
      std::string{handle}};
  }

  auto* node = findNode(m_serialNumbers[parsed.k - 1]);
  if (!node)
  {
    throw CliError{
      ErrorCode::Gone,
      fmt::format("the node with the handle '{}' is no longer in the map", handle),
      std::string{handle}};
  }

  if (parsed.faceIndex)
  {
    const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(node);
    if (!brushNode)
    {
      throw invalidHandle(
        handle, fmt::format("only brushes have faces, not {}", nodePath(*node)));
    }
    if (*parsed.faceIndex >= brushNode->brush().faceCount())
    {
      throw CliError{
        ErrorCode::NodeNotFound,
        fmt::format(
          "no face at '{}': the brush has {} faces",
          handle,
          brushNode->brush().faceCount()),
        std::string{handle}};
    }
  }
  return {node, parsed.faceIndex};
}

mdl::Node* NodeHandleTable::findNode(const uint64_t serialNumber)
{
  if (!m_indexValid)
  {
    m_nodes.clear();
    indexNodes(m_map.worldNode(), m_nodes);
    m_indexValid = true;
  }

  const auto it = m_nodes.find(serialNumber);
  return it != m_nodes.end() ? it->second : nullptr;
}

NodeOrFace resolveNodeReference(
  mdl::WorldNode& worldNode, const std::string_view reference, NodeHandleTable* handles)
{
  return handles && isNodeHandle(reference) ? handles->resolve(reference)
                                            : resolveNodePath(worldNode, reference);
}

std::string handleKey(const std::string_view key, const bool isList)
{
  if (key == "path")
  {
    return "handle";
  }
  return fmt::format("{}_{}", key, isList ? "handles" : "handle");
}

void setNodePath(
  Json& object,
  const std::string_view key,
  const mdl::Node& node,
  NodeHandleTable* handles)
{
  setNodePath(object, key, nodePath(node), node, handles);
}

void setNodePath(
  Json& object,
  const std::string_view key,
  std::string path,
  const mdl::Node& node,
  NodeHandleTable* handles)
{
  object.set(std::string{key}, std::move(path));
  if (handles)
  {
    object.set(handleKey(key), handles->handle(node));
  }
}

void setFacePath(
  Json& object,
  const std::string_view key,
  const mdl::BrushNode& brushNode,
  const size_t faceIndex,
  NodeHandleTable* handles)
{
  object.set(std::string{key}, facePath(brushNode, faceIndex));
  if (handles)
  {
    object.set(handleKey(key), handles->faceHandle(brushNode, faceIndex));
  }
}

NodePathList::NodePathList(NodeHandleTable* handles)
  : m_handleTable{handles}
{
}

void NodePathList::push(std::string path, const mdl::Node& node)
{
  m_paths.push(std::move(path));
  if (m_handleTable)
  {
    m_handles.push(m_handleTable->handle(node));
  }
}

void NodePathList::push(const mdl::Node& node)
{
  push(nodePath(node), node);
}

size_t NodePathList::size() const
{
  return m_paths.size();
}

const Json& NodePathList::paths() const
{
  return m_paths;
}

const Json& NodePathList::handles() const
{
  return m_handles;
}

void NodePathList::setOn(Json& object, const std::string_view key) &&
{
  object.set(std::string{key}, std::move(m_paths));
  if (m_handleTable)
  {
    object.set(handleKey(key, true), std::move(m_handles));
  }
}

} // namespace tb::cli
