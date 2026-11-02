/*
 Copyright (C) 2010 Kristian Duske

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

#include "mdl/PortalFile.h"

#include "kd/filesystem_utils.h"
#include "kd/ranges/to.h"
#include "kd/reflection_impl.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"

#include "vm/polygon.h"
#include "vm/polygon_io.h" // IWYU pragma: keep
#include "vm/vec.h"

#include <fmt/format.h>

#include <array>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>

namespace tb::mdl
{

kdl_reflect_impl(PortalGraphPortal);
kdl_reflect_impl(PortalGraph);

namespace
{

constexpr auto Whitespace = " \t\r\n";
constexpr auto PointDelimiters = "() \t\r\n";

/**
 * Graph parses the whole file and validates the cell indices. PolygonsOnly is the
 * lenient mode of the portal viewer: it stops after the portal lines and ignores the cell
 * indices, so that it accepts every file that the viewer has always accepted.
 */
enum class Mode
{
  Graph,
  PolygonsOnly,
};

class ParseError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

class LineReader
{
private:
  std::istream& m_stream;
  size_t m_lineNumber = 0;
  std::optional<std::string> m_unread;

public:
  explicit LineReader(std::istream& stream)
    : m_stream{stream}
  {
  }

  [[noreturn]] void fail(const std::string& message) const
  {
    throw ParseError{fmt::format("Line {}: {}", m_lineNumber, message)};
  }

  std::optional<std::string> tryNext()
  {
    if (m_unread)
    {
      auto line = std::move(m_unread);
      m_unread.reset();
      return line;
    }

    ++m_lineNumber;
    auto line = std::string{};
    if (!std::getline(m_stream, line))
    {
      return std::nullopt;
    }
    return line;
  }

  std::string next()
  {
    if (auto line = tryNext())
    {
      return std::move(*line);
    }
    fail("Unexpected end of file");
  }

  /** Puts back the line returned by the last call to next(). */
  void unread(std::string line) { m_unread = std::move(line); }
};

size_t parseSize(const LineReader& reader, const std::string_view str)
{
  if (const auto value = kdl::str_to_size(kdl::str_trim(str, Whitespace)))
  {
    return *value;
  }
  reader.fail("Expected a number");
}

int parseInt(const LineReader& reader, const std::string_view str)
{
  if (const auto value = kdl::str_to_int(str))
  {
    return *value;
  }
  reader.fail("Expected a number");
}

float parseFloat(const LineReader& reader, const std::string_view str)
{
  if (const auto value = kdl::str_to_float(str))
  {
    return *value;
  }
  reader.fail("Expected a number");
}

size_t parseSizeLine(LineReader& reader)
{
  return parseSize(reader, reader.next());
}

/**
 * Parses a portal line: the number of points, the indices of the two cells, the hint
 * flag (Quake 3 only) and the points in parentheses.
 */
PortalGraphPortal parsePortal(
  LineReader& reader, const size_t cellCount, const bool hasHintFlag, const Mode mode)
{
  const auto line = reader.next();
  const auto pointsStart = std::min(line.find('('), line.size());
  const auto header = kdl::str_split(line.substr(0, pointsStart), Whitespace);
  const auto coords = kdl::str_split(line.substr(pointsStart), PointDelimiters);

  if (header.size() != (hasHintFlag ? 4u : 3u))
  {
    reader.fail("Expected portal header");
  }

  const auto pointCount = parseSize(reader, header[0]);
  auto cells = std::array<size_t, 2>{};
  if (mode == Mode::Graph)
  {
    for (size_t i = 0; i < 2; ++i)
    {
      cells[i] = parseSize(reader, header[i + 1]);
      if (cells[i] >= cellCount)
      {
        reader.fail(fmt::format("Cell index {} out of range", cells[i]));
      }
    }
  }

  const auto hasExpectedCoords = mode == Mode::Graph ? coords.size() == 3 * pointCount
                                                     : coords.size() >= 3 * pointCount;
  if (!hasExpectedCoords)
  {
    reader.fail(fmt::format("Expected {} points", pointCount));
  }

  auto vertices = std::vector<vm::vec3f>{};
  vertices.reserve(pointCount);
  for (size_t i = 0; i < pointCount; ++i)
  {
    vertices.emplace_back(
      parseFloat(reader, coords[3 * i]),
      parseFloat(reader, coords[3 * i + 1]),
      parseFloat(reader, coords[3 * i + 2]));
  }

  return PortalGraphPortal{vm::polygon3f{std::move(vertices)}, cells[0], cells[1]};
}

std::vector<PortalGraphPortal> parsePortals(
  LineReader& reader,
  const size_t portalCount,
  const size_t cellCount,
  const bool hasHintFlag,
  const Mode mode)
{
  auto portals = std::vector<PortalGraphPortal>{};
  portals.reserve(portalCount);
  for (size_t i = 0; i < portalCount; ++i)
  {
    portals.push_back(parsePortal(reader, cellCount, hasHintFlag, mode));
  }
  return portals;
}

/**
 * Parses the PRT2 trailer: for each cluster, a line with the indices of its leafs,
 * terminated by -1.
 */
std::vector<size_t> parsePrt2Clusters(
  LineReader& reader, const size_t leafCount, const size_t clusterCount)
{
  auto clusterOfLeaf = std::vector<std::optional<size_t>>(leafCount);

  for (size_t cluster = 0; cluster < clusterCount; ++cluster)
  {
    const auto tokens = kdl::str_split(reader.next(), Whitespace);
    if (tokens.empty() || tokens.back() != "-1")
    {
      reader.fail("Expected -1");
    }

    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
      const auto leaf = parseInt(reader, tokens[i]);
      if (leaf < 0 || size_t(leaf) >= leafCount)
      {
        reader.fail(fmt::format("Leaf index {} out of range", leaf));
      }

      auto& assignedCluster = clusterOfLeaf[size_t(leaf)];
      if (assignedCluster)
      {
        reader.fail(fmt::format("Leaf {} is in more than one cluster", leaf));
      }
      assignedCluster = cluster;
    }
  }

  auto result = std::vector<size_t>{};
  result.reserve(leafCount);
  for (size_t leaf = 0; leaf < leafCount; ++leaf)
  {
    if (!clusterOfLeaf[leaf])
    {
      // reported at the last line of the trailer
      reader.fail(fmt::format("Leaf {} is not in any cluster", leaf));
    }
    result.push_back(*clusterOfLeaf[leaf]);
  }
  return result;
}

/**
 * Parses the PRT1-AM trailer: for each leaf, a line with the index of its cluster.
 */
std::vector<size_t> parsePrt1AmClusters(
  LineReader& reader, const size_t leafCount, const size_t clusterCount)
{
  auto result = std::vector<size_t>{};
  result.reserve(leafCount);
  for (size_t leaf = 0; leaf < leafCount; ++leaf)
  {
    const auto cluster = parseSizeLine(reader);
    if (cluster >= clusterCount)
    {
      reader.fail(fmt::format("Cluster index {} out of range", cluster));
    }
    result.push_back(cluster);
  }
  return result;
}

PortalGraph parsePrt1(LineReader& reader, const Mode mode)
{
  const auto leafCount = parseSizeLine(reader);
  const auto portalCount = parseSizeLine(reader);

  // In the Quake 3 variant, the next line holds a single value, the number of solid
  // faces, which follow the portals and are ignored. Otherwise, the next line is the
  // first portal.
  auto hasHintFlag = false;
  if (portalCount > 0)
  {
    auto line = reader.next();
    if (kdl::str_split(line, Whitespace).size() == 1)
    {
      hasHintFlag = true;
    }
    else
    {
      reader.unread(std::move(line));
    }
  }

  auto portals = parsePortals(reader, portalCount, leafCount, hasHintFlag, mode);
  auto clusterOfLeaf =
    std::views::iota(size_t{0}, leafCount) | kdl::ranges::to<std::vector>();

  return PortalGraph{leafCount, leafCount, std::move(portals), std::move(clusterOfLeaf)};
}

using ParseClusters = std::vector<size_t> (*)(LineReader&, size_t, size_t);

/**
 * Parses the portals and the cluster trailer of a PRT2 or PRT1-AM file, whose portals
 * connect clusters.
 */
PortalGraph parseClusteredPortals(
  LineReader& reader,
  const size_t leafCount,
  const size_t clusterCount,
  const size_t portalCount,
  const ParseClusters parseClusters,
  const Mode mode)
{
  auto portals = parsePortals(reader, portalCount, clusterCount, false, mode);
  auto clusterOfLeaf = mode == Mode::Graph
                         ? parseClusters(reader, leafCount, clusterCount)
                         : std::vector<size_t>{};

  return PortalGraph{
    leafCount, clusterCount, std::move(portals), std::move(clusterOfLeaf)};
}

PortalGraph parsePrt2(LineReader& reader, const Mode mode)
{
  const auto leafCount = parseSizeLine(reader);
  const auto clusterCount = parseSizeLine(reader);
  const auto portalCount = parseSizeLine(reader);

  return parseClusteredPortals(
    reader, leafCount, clusterCount, portalCount, parsePrt2Clusters, mode);
}

PortalGraph parsePrt1Am(LineReader& reader, const Mode mode)
{
  const auto clusterCount = parseSizeLine(reader);
  const auto portalCount = parseSizeLine(reader);
  const auto leafCount = parseSizeLine(reader);

  return parseClusteredPortals(
    reader, leafCount, clusterCount, portalCount, parsePrt1AmClusters, mode);
}

Result<PortalGraph> parsePortalFile(std::istream& stream, const Mode mode)
{
  auto reader = LineReader{stream};

  try
  {
    const auto formatCode = kdl::str_trim(reader.tryNext().value_or(""), Whitespace);

    if (formatCode == "PRT1")
    {
      return parsePrt1(reader, mode);
    }
    if (formatCode == "PRT2")
    {
      return parsePrt2(reader, mode);
    }
    if (formatCode == "PRT1-AM")
    {
      return parsePrt1Am(reader, mode);
    }
    return Error{"Unknown portal format: " + formatCode};
  }
  catch (const ParseError& e)
  {
    return Error{e.what()};
  }
}

} // namespace

bool canLoadPortalFile(const std::filesystem::path& path)
{
  return kdl::with_istream(
           path, [](auto& stream) { return stream.is_open() && stream.good(); })
         | kdl::transform_error([](const auto&) { return false; }) | kdl::value();
}

Result<PortalGraph> loadPortalGraph(std::istream& stream)
{
  return parsePortalFile(stream, Mode::Graph);
}

Result<std::vector<vm::polygon3f>> loadPortalFile(std::istream& stream)
{
  return parsePortalFile(stream, Mode::PolygonsOnly) | kdl::transform([](auto graph) {
           return graph.portals | std::views::transform([](auto& portal) {
                    return std::move(portal.polygon);
                  })
                  | kdl::ranges::to<std::vector>();
         });
}

} // namespace tb::mdl
