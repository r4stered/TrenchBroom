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

#include "mdl/LoadBspLeafTree.h"

#include "fs/ReaderException.h"
#include "mdl/BspDirectory.h"

#include "kd/contracts.h"
#include "kd/reflection_impl.h"

#include "vm/bbox_io.h" // IWYU pragma: keep

#include <fmt/format.h>

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string>

namespace tb::mdl
{

std::ostream& operator<<(std::ostream& lhs, const BspLeafContents rhs)
{
  switch (rhs)
  {
  case BspLeafContents::Empty:
    return lhs << "Empty";
  case BspLeafContents::Solid:
    return lhs << "Solid";
  case BspLeafContents::Water:
    return lhs << "Water";
  case BspLeafContents::Slime:
    return lhs << "Slime";
  case BspLeafContents::Lava:
    return lhs << "Lava";
  case BspLeafContents::Sky:
    return lhs << "Sky";
  }
  return lhs << "Contents(" << static_cast<int32_t>(rhs) << ")";
}

kdl_reflect_impl(BspLeaf);

namespace
{
namespace BspLayout
{
constexpr int32_t Bsp2Version = 'B' | ('S' << 8) | ('P' << 16) | ('2' << 24);

constexpr size_t PlaneSize = 0x14;
constexpr size_t ModelHeadNode = 0x24;
constexpr size_t ModelVisLeafs = 0x34;

constexpr size_t Bsp29NodeSize = 0x18;
constexpr size_t Bsp29LeafSize = 0x1C;
constexpr size_t Bsp2NodeSize = 0x2C;
constexpr size_t Bsp2LeafSize = 0x2C;
} // namespace BspLayout

enum class BspFormat
{
  Bsp29,
  Bsp2,
};

class InvalidBspError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

struct Lump
{
  size_t offset;
  size_t length;
};

Lump readLump(fs::Reader& reader, const size_t address)
{
  reader.seekFromBegin(address);
  const auto offset = reader.readInt<int32_t>();
  const auto length = reader.readInt<int32_t>();
  if (offset < 0 || length < 0)
  {
    throw InvalidBspError{"Invalid lump"};
  }
  return {size_t(offset), size_t(length)};
}

std::vector<vm::plane3d> readPlanes(const fs::Reader& reader, const Lump& lump)
{
  const auto count = lump.length / BspLayout::PlaneSize;
  auto planeReader = reader.subReaderFromBegin(lump.offset, lump.length);

  auto result = std::vector<vm::plane3d>{};
  result.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    const auto normal = planeReader.readVec<float, 3>();
    const auto distance = planeReader.readFloat<float>();
    planeReader.seekForward(sizeof(int32_t)); // type
    result.emplace_back(double(distance), vm::vec3d{normal});
  }
  return result;
}

vm::vec3d readBoundsVec(fs::Reader& reader, const BspFormat format)
{
  auto result = vm::vec3d{};
  for (size_t i = 0; i < 3; ++i)
  {
    result[i] = format == BspFormat::Bsp29 ? double(reader.readInt<int16_t>())
                                           : double(reader.readFloat<float>());
  }
  return result;
}

using Child = BspLeafTree::Child;

/**
 * Decodes a child reference of a node or a model: a non-negative value is a node index,
 * and a negative value c is the leaf index -(c+1).
 */
Child decodeChild(const int32_t child)
{
  return child >= 0 ? Child{Child::Type::Node, size_t(child)}
                    : Child{Child::Type::Leaf, size_t(-(int64_t(child) + 1))};
}

struct RawNode
{
  size_t planeIndex;
  std::array<Child, 2> children;
};

std::vector<RawNode> readNodes(
  const fs::Reader& reader, const Lump& lump, const BspFormat format)
{
  const auto nodeSize =
    format == BspFormat::Bsp29 ? BspLayout::Bsp29NodeSize : BspLayout::Bsp2NodeSize;
  const auto count = lump.length / nodeSize;

  auto result = std::vector<RawNode>{};
  result.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    auto nodeReader = reader.subReaderFromBegin(lump.offset + i * nodeSize, nodeSize);
    const auto planeIndex = nodeReader.readInt<int32_t>();
    if (planeIndex < 0)
    {
      throw InvalidBspError{fmt::format("Invalid plane index in node {}", i)};
    }

    auto children = std::array<Child, 2>{};
    for (auto& child : children)
    {
      child = decodeChild(
        format == BspFormat::Bsp29 ? nodeReader.readInt<int16_t>()
                                   : nodeReader.readInt<int32_t>());
    }
    result.push_back({size_t(planeIndex), children});
  }
  return result;
}

std::vector<BspLeaf> readLeafs(
  const fs::Reader& reader, const Lump& lump, const BspFormat format, const size_t count)
{
  const auto leafSize =
    format == BspFormat::Bsp29 ? BspLayout::Bsp29LeafSize : BspLayout::Bsp2LeafSize;
  if (count * leafSize > lump.length)
  {
    throw InvalidBspError{
      fmt::format("Expected {} leafs, but the leafs lump is too small", count)};
  }

  auto result = std::vector<BspLeaf>{};
  result.reserve(count);
  for (size_t i = 0; i < count; ++i)
  {
    auto leafReader = reader.subReaderFromBegin(lump.offset + i * leafSize, leafSize);
    const auto contents = BspLeafContents{leafReader.readInt<int32_t>()};
    leafReader.seekForward(sizeof(int32_t)); // visofs
    const auto min = readBoundsVec(leafReader, format);
    const auto max = readBoundsVec(leafReader, format);
    result.push_back({contents, vm::bbox3d{min, max}});
  }
  return result;
}

/**
 * Checks that every child reachable from the root refers to a valid node or leaf and
 * that every node is reached only once, so that findLeaf terminates.
 */
void validateTree(
  const std::vector<BspLeafTree::Node>& nodes, const size_t leafCount, const Child root)
{
  auto visited = std::vector<bool>(nodes.size(), false);
  auto stack = std::vector<Child>{root};

  while (!stack.empty())
  {
    const auto child = stack.back();
    stack.pop_back();

    if (child.type == Child::Type::Leaf)
    {
      if (child.index >= leafCount)
      {
        throw InvalidBspError{fmt::format("Invalid leaf index {}", child.index)};
      }
      continue;
    }

    if (child.index >= nodes.size())
    {
      throw InvalidBspError{fmt::format("Invalid node index {}", child.index)};
    }
    if (visited[child.index])
    {
      throw InvalidBspError{
        fmt::format("Node {} is reached more than once", child.index)};
    }
    visited[child.index] = true;

    stack.push_back(nodes[child.index].children[0]);
    stack.push_back(nodes[child.index].children[1]);
  }
}

BspLeafTree readLeafTree(fs::Reader& reader)
{
  const auto version = reader.readInt<int32_t>();
  const auto format =
    isBsp29Or30(version) ? BspFormat::Bsp29
    : version == BspLayout::Bsp2Version
      ? BspFormat::Bsp2
      : throw InvalidBspError{fmt::format("Unsupported BSP version: {}", version)};

  const auto planesLump = readLump(reader, BspDirectory::PlanesAddress);
  const auto nodesLump = readLump(reader, BspDirectory::NodesAddress);
  const auto leafsLump = readLump(reader, BspDirectory::LeafsAddress);
  const auto modelsLump = readLump(reader, BspDirectory::ModelsAddress);

  auto modelReader = reader.subReaderFromBegin(
    modelsLump.offset, std::min(modelsLump.length, BspDirectory::ModelSize));
  modelReader.seekFromBegin(BspLayout::ModelHeadNode);
  const auto root = decodeChild(modelReader.readInt<int32_t>());
  modelReader.seekFromBegin(BspLayout::ModelVisLeafs);
  const auto visLeafCount = modelReader.readInt<int32_t>();
  if (visLeafCount < 0)
  {
    throw InvalidBspError{"Invalid vis leaf count"};
  }

  const auto planes = readPlanes(reader, planesLump);
  const auto rawNodes = readNodes(reader, nodesLump, format);

  auto nodes = std::vector<BspLeafTree::Node>{};
  nodes.reserve(rawNodes.size());
  for (size_t i = 0; i < rawNodes.size(); ++i)
  {
    if (rawNodes[i].planeIndex >= planes.size())
    {
      throw InvalidBspError{fmt::format("Invalid plane index in node {}", i)};
    }
    nodes.push_back({planes[rawNodes[i].planeIndex], rawNodes[i].children});
  }

  // leaf 0 is the shared solid leaf, followed by the vis leafs of the world model
  auto leafs = readLeafs(reader, leafsLump, format, size_t(visLeafCount) + 1);

  validateTree(nodes, leafs.size(), root);
  return BspLeafTree{std::move(nodes), std::move(leafs), root};
}

} // namespace

BspLeafTree::BspLeafTree(
  std::vector<Node> nodes, std::vector<BspLeaf> leafs, const Child root)
  : m_nodes{std::move(nodes)}
  , m_leafs{std::move(leafs)}
  , m_root{root}
{
}

const std::vector<BspLeaf>& BspLeafTree::leafs() const
{
  return m_leafs;
}

size_t BspLeafTree::findLeaf(const vm::vec3d& point) const
{
  auto child = m_root;
  while (child.type == Child::Type::Node)
  {
    contract_assert(child.index < m_nodes.size());

    const auto& node = m_nodes[child.index];
    child = node.plane.point_distance(point) > 0.0 ? node.children[0] : node.children[1];
  }

  contract_assert(child.index < m_leafs.size());
  return child.index;
}

Result<BspLeafTree> loadBspLeafTree(fs::Reader reader)
{
  try
  {
    return readLeafTree(reader);
  }
  catch (const InvalidBspError& e)
  {
    return Error{e.what()};
  }
  catch (const fs::ReaderException& e)
  {
    return Error{e.what()};
  }
}

} // namespace tb::mdl
