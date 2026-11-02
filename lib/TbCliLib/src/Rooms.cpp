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

#include "cli/Rooms.h"

#include "cli/CliError.h"
#include "cli/Compile.h"
#include "cli/Config.h"
#include "cli/Describe.h"
#include "cli/FileUtils.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/LoadMap.h"
#include "cli/MapContents.h"
#include "cli/NodePath.h"
#include "cli/Query.h"
#include "cli/Verify.h"
#include "fs/Reader.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/LoadBspLeafTree.h"
#include "mdl/Map.h"
#include "mdl/PortalFile.h"
#include "mdl/WorldNode.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/** How far apart the planes of two portals may be and still count as one plane. */
constexpr auto PlaneDistanceEpsilon = 0.01;

/** How finely plane normals are compared. */
constexpr auto PlaneNormalPrecision = 1e4;

/** Portals with a smaller area are left out: they have no usable plane. */
constexpr auto MinPortalArea = 1e-6;

/** The width and length of the space a player needs, as the guide's conventions say. */
constexpr auto PlayerWidth = 32.0;

/** The height of the space a player needs, as the guide's conventions say. */
constexpr auto PlayerHeight = 56.0;

/**
 * The flags of the qbsp run that writes the cells. `-nodetail` makes detail brushes and
 * liquids structural, so that every leaf is its own cell and a liquid's surface is a
 * portal; without it, qbsp groups the leafs of detail and liquids into clusters.
 * `-forceprt1` keeps the portal file PRT1 in any case. The portal file is not meant for
 * vis.
 */
const auto CellsQbspFlags = std::vector<std::string>{"-nodetail", "-forceprt1"};

/**
 * The flags of the qbsp run that looks for a leak: none, so that the map is sealed as
 * verify's qbsp run seals it. Detail brushes do not seal a map, but with `-nodetail`
 * they would.
 */
const auto LeakCheckQbspFlags = std::vector<std::string>{};

/** The `.bsp` and `.prt` that rooms reads, whether compiled for it or reused. */
struct CompiledMap
{
  mdl::BspLeafTree tree;
  mdl::PortalGraph graph;
};

/**
 * Reads the `.bsp`, the `.prt` and the sidecar of a compile from the given directory.
 * Every problem is thrown with the given code.
 */
CompiledMap readCompiledMap(
  const std::filesystem::path& dir,
  const std::string& mapName,
  const std::string& rev,
  const ErrorCode code)
{
  const auto read = [&](const std::string_view suffix) {
    const auto path = dir / (mapName + std::string{suffix});
    auto bytes = readFile(path);
    if (!bytes)
    {
      throw CliError{code, fmt::format("cannot read {}", path)};
    }
    return std::move(*bytes);
  };
  const auto unreadable = [&](const std::string_view suffix, const auto& result) {
    return CliError{
      code,
      fmt::format(
        "cannot read {}{}: {}", mapName, suffix, std::get<Error>(result.error()).msg)};
  };

  const auto sidecarName = mapName + ".bsp.tbcli.json";
  const auto sidecar = parseJson(read(".bsp.tbcli.json"));
  if (
    !sidecar || !sidecar.value()["rev"].isString() || !sidecar.value()["leaked"].isBool())
  {
    throw CliError{
      code, fmt::format("{} in {} is not a tbcli sidecar", sidecarName, dir)};
  }
  if (const auto& sidecarRev = sidecar.value()["rev"].asString(); sidecarRev != rev)
  {
    throw CliError{
      code,
      fmt::format(
        "the artifacts in {} are of rev {}, not of the map's rev {}",
        dir,
        sidecarRev,
        rev)};
  }
  if (sidecar.value()["leaked"].asBool())
  {
    throw CliError{
      code, fmt::format("the artifacts in {} are of a compile that leaked", dir)};
  }

  const auto bsp = read(".bsp");
  auto tree = mdl::loadBspLeafTree(fs::Reader::from(bsp.data(), bsp.data() + bsp.size()));
  if (tree.is_error())
  {
    throw unreadable(".bsp", tree);
  }

  auto prt = std::istringstream{read(".prt")};
  auto graph = mdl::loadPortalGraph(prt);
  if (graph.is_error())
  {
    throw unreadable(".prt", graph);
  }

  const auto& portalGraph = graph.value();
  const auto leafLevel =
    portalGraph.clusterCount == portalGraph.leafCount
    && std::ranges::equal(
      portalGraph.clusterOfLeaf, std::views::iota(size_t{0}, portalGraph.leafCount));
  if (!leafLevel)
  {
    throw CliError{
      code,
      fmt::format(
        "{}.prt groups leafs into clusters, as qbsp does for detail and liquids, but "
        "rooms needs one cell per leaf, as qbsp -nodetail writes{}",
        mapName,
        code == ErrorCode::StaleArtifact
          ? "; run rooms without --from-artifacts, which runs qbsp so"
          : "")};
  }
  if (portalGraph.leafCount + 1 != tree.value().leafs().size())
  {
    throw CliError{
      code,
      fmt::format(
        "{}.prt has {} leafs, but {}.bsp has {}",
        mapName,
        portalGraph.leafCount,
        mapName,
        tree.value().leafs().size() - 1)};
  }

  return {std::move(tree).value(), std::move(graph).value()};
}

/** Fails with `compile_failed` if qbsp failed or was stopped. */
void checkQbspRun(const CompileResult& result)
{
  // qbsp is the only tool that runs
  const auto& run = result.runs.front();
  if (run.failed())
  {
    const auto error =
      std::ranges::find(run.messages, Severity::Error, &CompilerMessage::severity);
    throw CliError{
      ErrorCode::CompileFailed,
      error != run.messages.end()
        ? fmt::format("{}: {}", run.failureMessage(), error->message)
        : run.failureMessage()};
  }
}

/**
 * Runs qbsp twice, first with LeakCheckQbspFlags to find a leak as verify does, then
 * with CellsQbspFlags, and reads what the second run wrote.
 */
CompiledMap compileForRooms(
  LoadedMap& loadedMap, const Config& config, const RoomsOptions& options)
{
  const auto name = mapName(loadedMap);
  const auto artifacts = TemporaryDirectory{"tbcli-rooms-"};

  auto compileOptions = CompileOptions{};
  compileOptions.compilers = config.compilers;
  compileOptions.timeout = options.timeout;
  for (const auto* flags : {&LeakCheckQbspFlags, &CellsQbspFlags})
  {
    compileOptions.qbspFlags = *flags;
    if (flags == &CellsQbspFlags)
    {
      compileOptions.artifacts = artifacts.path();
    }

    const auto result = compile(loadedMap.bytes, name, loadedMap.rev, compileOptions);
    if (result.leaked)
    {
      throw CliError{
        ErrorCode::MapLeaks,
        "the map leaks, so it has no rooms; fix the leak that the issue describes"}
        .withIssue(leakIssueToJson(loadedMap, config.verify, result));
    }
    checkQbspRun(result);
  }

  return readCompiledMap(artifacts.path(), name, loadedMap.rev, ErrorCode::CompileFailed);
}

/** The name of the contents of a cell: empty, water, slime or lava. */
std::string_view toString(const mdl::BspLeafContents contents)
{
  switch (contents)
  {
  case mdl::BspLeafContents::Water:
    return "water";
  case mdl::BspLeafContents::Slime:
    return "slime";
  case mdl::BspLeafContents::Lava:
    return "lava";
  case mdl::BspLeafContents::Empty:
  case mdl::BspLeafContents::Solid:
  case mdl::BspLeafContents::Sky:
    break;
  }
  return "empty";
}

/**
 * The contents of the cell of a leaf with the given contents: those of a liquid, or else
 * empty; nullopt for solid and sky leafs, which are no cells.
 */
std::optional<mdl::BspLeafContents> cellContentsOf(const mdl::BspLeafContents contents)
{
  switch (contents)
  {
  case mdl::BspLeafContents::Solid:
  case mdl::BspLeafContents::Sky:
    return std::nullopt;
  case mdl::BspLeafContents::Water:
  case mdl::BspLeafContents::Slime:
  case mdl::BspLeafContents::Lava:
    return contents;
  case mdl::BspLeafContents::Empty:
    break;
  }
  return mdl::BspLeafContents::Empty;
}

/** A vis leaf of the portal file. */
struct Cell
{
  vm::bbox3d bounds;
  /** Empty, water, slime or lava. */
  mdl::BspLeafContents contents;
};

struct Plane
{
  /** The normal, whose component of the largest magnitude is positive. */
  vm::vec3d normal;
  double distance;
};

/** A side of a plane: the side its normal points to, or the other. */
enum class Side
{
  Front,
  Back,
};

Side opposite(const Side side)
{
  return side == Side::Front ? Side::Back : Side::Front;
}

/** A plane, by its id, and a side of it. */
using PlaneSideKey = std::pair<size_t, Side>;

/** The planes of the portals and of the cells' bounds, each with one id. */
class Planes
{
private:
  std::vector<Plane> m_planes;
  std::map<std::array<int64_t, 4>, size_t> m_ids;

public:
  /** The id of the plane, which is the same for both of its orientations. */
  size_t id(vm::vec3d normal, double distance)
  {
    const auto major = vm::find_abs_max_component(normal);
    if (normal[major] < 0.0)
    {
      normal = -normal;
      distance = -distance;
    }
    const auto key = std::array<int64_t, 4>{
      std::llround(normal.x() * PlaneNormalPrecision),
      std::llround(normal.y() * PlaneNormalPrecision),
      std::llround(normal.z() * PlaneNormalPrecision),
      std::llround(distance / PlaneDistanceEpsilon),
    };
    const auto [it, inserted] = m_ids.try_emplace(key, m_planes.size());
    if (inserted)
    {
      m_planes.push_back({normal, distance});
    }
    return it->second;
  }

  const Plane& operator[](const size_t id) const { return m_planes[id]; }

  /** The side of the plane that the given point is on. */
  Side side(const size_t id, const vm::vec3d& point) const
  {
    const auto& plane = m_planes[id];
    return vm::dot(plane.normal, point) >= plane.distance ? Side::Front : Side::Back;
  }
};

struct Portal
{
  size_t a;
  size_t b;
  size_t plane;
  /** The sides of the plane that `a` and `b` are on, which are opposite. */
  Side sideA;
  Side sideB;
  double area;
  vm::vec3d center;
};

/** The area of a polygon and its unit normal, by Newell's method. */
std::pair<double, vm::vec3d> areaAndNormal(const std::vector<vm::vec3f>& vertices)
{
  auto sum = vm::vec3d{0, 0, 0};
  for (size_t i = 0; i < vertices.size(); ++i)
  {
    const auto current = vm::vec3d{vertices[i]};
    const auto next = vm::vec3d{vertices[(i + 1) % vertices.size()]};
    sum = sum
          + vm::vec3d{
            (current.y() - next.y()) * (current.z() + next.z()),
            (current.z() - next.z()) * (current.x() + next.x()),
            (current.x() - next.x()) * (current.y() + next.y()),
          };
  }
  const auto length = vm::length(sum);
  return {length / 2.0, length > 0.0 ? sum / length : sum};
}

/** The area of the given box seen along the given unit normal. */
double crossSection(const vm::bbox3d& bounds, const vm::vec3d& normal)
{
  const auto size = bounds.size();
  return std::abs(normal.x()) * size.y() * size.z()
         + std::abs(normal.y()) * size.x() * size.z()
         + std::abs(normal.z()) * size.x() * size.y();
}

/** The extent of the given box along the given unit direction. */
double extentAlong(const vm::bbox3d& bounds, const vm::vec3d& direction)
{
  const auto size = bounds.size();
  return std::abs(direction.x()) * size.x() + std::abs(direction.y()) * size.y()
         + std::abs(direction.z()) * size.z();
}

/** The shorter of the box's extents along two perpendicular directions in the plane. */
double minExtentInPlane(const vm::bbox3d& bounds, const vm::vec3d& normal)
{
  auto axis = vm::vec3d{0, 0, 0};
  // the axis least aligned with the normal
  axis[vm::find_abs_max_component(normal, 2)] = 1.0;
  const auto u = vm::normalize(vm::cross(normal, axis));
  const auto v = vm::cross(normal, u);
  return std::min(extentAlong(bounds, u), extentAlong(bounds, v));
}

/** Whether a player could not stand in a space with the given bounds. */
bool isTooSmall(const vm::bbox3d& bounds)
{
  const auto size = bounds.size();
  return size.x() < PlayerWidth || size.y() < PlayerWidth || size.z() < PlayerHeight;
}

/**
 * How open a space is on one side of a plane: the cross-section of its cells that lie
 * against the plane on that side, and the area of their portals in the plane.
 */
struct PlaneOpening
{
  double crossSection = 0.0;
  double open = 0.0;

  PlaneOpening& operator+=(const PlaneOpening& other)
  {
    crossSection += other.crossSection;
    open += other.open;
    return *this;
  }

  /** The share of the cross-section that is open. */
  double ratio() const { return crossSection > 0.0 ? open / crossSection : 1.0; }
};

using PlaneOpenings = std::map<PlaneSideKey, PlaneOpening>;

/** The cells, the portals between them and the planes of both. */
struct CellGraph
{
  /** Indexed by vis leaf; unset for leafs that are no cells, such as sky. */
  std::vector<std::optional<Cell>> cells;
  /** The portals between two cells, in file order. */
  std::vector<Portal> portals;
  Planes planes;
  /** Per cell, how open it is on each side of each plane it lies against. */
  std::vector<PlaneOpenings> openings;
};

CellGraph buildCellGraph(const CompiledMap& compiled)
{
  auto result = CellGraph{};
  const auto& leafs = compiled.tree.leafs();
  for (size_t i = 0; i < compiled.graph.leafCount; ++i)
  {
    // vis leaf i is BSP leaf i + 1
    const auto& leaf = leafs[i + 1];
    if (const auto contents = cellContentsOf(leaf.contents))
    {
      result.cells.push_back(Cell{leaf.bounds, *contents});
    }
    else
    {
      result.cells.emplace_back();
    }
  }
  result.openings.resize(result.cells.size());

  // a cell lies against the planes of its bounds, even where walls close them
  for (size_t i = 0; i < result.cells.size(); ++i)
  {
    if (const auto& cell = result.cells[i])
    {
      for (size_t axis = 0; axis < 3; ++axis)
      {
        auto normal = vm::vec3d{0, 0, 0};
        normal[axis] = 1.0;
        const auto section = crossSection(cell->bounds, normal);
        const auto minPlane = result.planes.id(normal, cell->bounds.min[axis]);
        const auto maxPlane = result.planes.id(normal, cell->bounds.max[axis]);
        result.openings[i][{minPlane, Side::Front}].crossSection = section;
        result.openings[i][{maxPlane, Side::Back}].crossSection = section;
      }
    }
  }

  for (const auto& portal : compiled.graph.portals)
  {
    if (!result.cells[portal.a] || !result.cells[portal.b])
    {
      continue;
    }
    const auto [area, normal] = areaAndNormal(portal.polygon.vertices());
    if (area < MinPortalArea)
    {
      continue;
    }

    const auto center = vm::vec3d{portal.polygon.center()};
    const auto plane = result.planes.id(normal, vm::dot(normal, center));
    const auto& planeNormal = result.planes[plane].normal;

    // the centre of a wedge-shaped leaf's bounds can lie in the plane, so the sides
    // come from the offset between the centres
    const auto centerA = result.cells[portal.a]->bounds.center();
    const auto offset =
      vm::dot(planeNormal, centerA - result.cells[portal.b]->bounds.center());
    const auto sideA = offset > 0.0   ? Side::Front
                       : offset < 0.0 ? Side::Back
                                      : result.planes.side(plane, centerA);
    const auto sideB = opposite(sideA);
    result.portals.push_back({portal.a, portal.b, plane, sideA, sideB, area, center});

    for (const auto& [cell, side] :
         {std::pair{portal.a, sideA}, std::pair{portal.b, sideB}})
    {
      auto& opening = result.openings[cell][{plane, side}];
      opening.crossSection = crossSection(result.cells[cell]->bounds, planeNormal);
      opening.open += area;
    }
  }
  return result;
}

vm::bbox3d boundsOf(const CellGraph& graph, const std::vector<size_t>& cells)
{
  auto result = graph.cells[cells.front()]->bounds;
  for (const auto cell : cells)
  {
    result = vm::merge(result, graph.cells[cell]->bounds);
  }
  return result;
}

/** Cells merged so far: a union-find over the cells, with each group's openings. */
class CellGroups
{
private:
  struct Group
  {
    std::vector<size_t> cells;
    PlaneOpenings openings;
  };

  std::vector<size_t> m_parent;
  /** Valid at the roots only. */
  std::vector<Group> m_groups;

public:
  explicit CellGroups(const CellGraph& graph)
    : m_parent(graph.cells.size())
    , m_groups(graph.cells.size())
  {
    std::iota(m_parent.begin(), m_parent.end(), size_t{0});
    for (size_t i = 0; i < graph.cells.size(); ++i)
    {
      m_groups[i] = {{i}, graph.openings[i]};
    }
  }

  size_t find(size_t cell)
  {
    while (m_parent[cell] != cell)
    {
      m_parent[cell] = m_parent[m_parent[cell]];
      cell = m_parent[cell];
    }
    return cell;
  }

  void merge(const size_t a, const size_t b)
  {
    auto root = find(a);
    auto other = find(b);
    if (root == other)
    {
      return;
    }
    if (m_groups[root].cells.size() < m_groups[other].cells.size())
    {
      std::swap(root, other);
    }
    m_parent[other] = root;
    auto& group = m_groups[root];
    auto& merged = m_groups[other];
    group.cells.insert(group.cells.end(), merged.cells.begin(), merged.cells.end());
    for (const auto& [key, opening] : merged.openings)
    {
      group.openings[key] += opening;
    }
    merged = {};
  }

  /** How open the group of the given cell is on the given side of a plane. */
  double ratio(const size_t cell, const PlaneSideKey& key)
  {
    const auto& openings = m_groups[find(cell)].openings;
    const auto it = openings.find(key);
    return it != openings.end() ? it->second.ratio() : 1.0;
  }

  /** The cells of the group of the given cell. */
  const std::vector<size_t>& cells(const size_t cell)
  {
    return m_groups[find(cell)].cells;
  }
};

/** A portal through which a group meets another group. */
struct Boundary
{
  const Portal* portal;
  /** The cell of the group that the portal belongs to. */
  size_t cell;
  /** The side of the portal's plane that the group is on. */
  Side side;
  /** The root of the other group. */
  size_t neighbour;
};

/** Merges the cells of a cell graph into the groups that become rooms. */
class RoomMerger
{
private:
  const CellGraph& m_graph;
  double m_splitRatio;
  CellGroups m_groups;

public:
  RoomMerger(const CellGraph& graph, const double splitRatio)
    : m_graph{graph}
    , m_splitRatio{splitRatio}
    , m_groups{graph}
  {
  }

  /**
   * Merges the groups across every portal that is open enough, then lets the minor
   * groups join their neighbours, and returns the groups.
   */
  CellGroups merge() &&
  {
    mergeOpenPortals();
    absorbMinorGroups();
    return std::move(m_groups);
  }

private:
  /** The contents of the given group's cells, which all have the same. */
  mdl::BspLeafContents contentsOf(const size_t root) const
  {
    return m_graph.cells[root]->contents;
  }

  /**
   * How open a portal is, as the less open of the groups on its sides, or nullopt if it
   * joins a group to itself or two cells of different contents.
   */
  std::optional<double> openness(const Portal& portal)
  {
    if (
      m_groups.find(portal.a) == m_groups.find(portal.b)
      || contentsOf(portal.a) != contentsOf(portal.b))
    {
      return std::nullopt;
    }
    return std::min(
      m_groups.ratio(portal.a, {portal.plane, portal.sideA}),
      m_groups.ratio(portal.b, {portal.plane, portal.sideB}));
  }

  /**
   * Merges the groups across every portal that is open enough, the most open first, and
   * judges each portal against the groups merged so far, so that a doorway that is wide
   * compared to the leafs beside it is still narrow compared to the rooms beside it.
   */
  void mergeOpenPortals()
  {
    for (auto merged = true; merged;)
    {
      merged = false;

      auto order = std::vector<std::pair<double, size_t>>{};
      for (size_t i = 0; i < m_graph.portals.size(); ++i)
      {
        if (const auto score = openness(m_graph.portals[i]))
        {
          order.emplace_back(*score, i);
        }
      }
      std::ranges::stable_sort(order, std::greater{}, &std::pair<double, size_t>::first);

      for (const auto& entry : order)
      {
        // merges made in this pass may have made the portal less open
        const auto& portal = m_graph.portals[entry.second];
        if (const auto score = openness(portal); score && *score >= m_splitRatio)
        {
          m_groups.merge(portal.a, portal.b);
          merged = true;
        }
      }
    }
  }

  /** The boundaries of every group that meets another, by the group's root. */
  std::map<size_t, std::vector<Boundary>> boundaries()
  {
    auto result = std::map<size_t, std::vector<Boundary>>{};
    for (const auto& portal : m_graph.portals)
    {
      const auto rootA = m_groups.find(portal.a);
      const auto rootB = m_groups.find(portal.b);
      if (rootA != rootB)
      {
        result[rootA].push_back({&portal, portal.a, portal.sideA, rootB});
        result[rootB].push_back({&portal, portal.b, portal.sideB, rootA});
      }
    }
    return result;
  }

  /**
   * Whether a group is only a passage between groups, such as the leaf inside a
   * doorway: it has the same contents as its neighbours, is open where it meets them,
   * and is no deeper than it is wide there.
   */
  bool isPassage(const vm::bbox3d& bounds, const std::vector<Boundary>& boundaries)
  {
    return std::ranges::all_of(boundaries, [&](const Boundary& boundary) {
      const auto& portal = *boundary.portal;
      const auto& normal = m_graph.planes[portal.plane].normal;
      return contentsOf(portal.a) == contentsOf(portal.b)
             && m_groups.ratio(boundary.cell, {portal.plane, boundary.side})
                  >= m_splitRatio
             && extentAlong(bounds, normal) <= minExtentInPlane(bounds, normal);
    });
  }

  /**
   * Lets every minor group join a neighbouring group of the same contents: a group that
   * is only a passage, or that is too small for a player, such as a recess or the gap
   * between a wall and a pillar. Each joins the neighbour with the lowest cell among the
   * groups that are not minor; one whose neighbours are all minor waits until one of
   * them has joined such a group. Minor groups that are left then merge with each
   * other, so that a space made only of minor groups, such as a crawlspace, is one room.
   */
  void absorbMinorGroups()
  {
    auto minorCells = std::vector<bool>(m_graph.cells.size(), false);
    for (const auto& [root, groupBoundaries] : boundaries())
    {
      const auto bounds = boundsOf(m_graph, m_groups.cells(root));
      if (isTooSmall(bounds) || isPassage(bounds, groupBoundaries))
      {
        for (const auto cell : m_groups.cells(root))
        {
          minorCells[cell] = true;
        }
      }
    }

    const auto isMinor = [&](const size_t root) {
      return std::ranges::all_of(
        m_groups.cells(root), [&](const size_t cell) { return minorCells[cell]; });
    };
    const auto lowestCell = [&](const size_t root) {
      return std::ranges::min(m_groups.cells(root));
    };

    for (auto absorbed = true; absorbed;)
    {
      absorbed = false;
      auto absorptions = std::vector<std::pair<size_t, size_t>>{};
      for (const auto& [root, groupBoundaries] : boundaries())
      {
        if (!isMinor(root))
        {
          continue;
        }
        auto target = std::optional<size_t>{};
        for (const auto& boundary : groupBoundaries)
        {
          if (
            contentsOf(boundary.neighbour) == contentsOf(root)
            && !isMinor(boundary.neighbour)
            && (!target || lowestCell(boundary.neighbour) < lowestCell(*target)))
          {
            target = boundary.neighbour;
          }
        }
        if (target)
        {
          absorptions.emplace_back(root, *target);
        }
      }

      for (const auto& [minor, target] : absorptions)
      {
        m_groups.merge(minor, target);
        absorbed = true;
      }
    }

    for (const auto& [root, groupBoundaries] : boundaries())
    {
      for (const auto& boundary : groupBoundaries)
      {
        if (
          isMinor(root) && isMinor(boundary.neighbour)
          && contentsOf(boundary.neighbour) == contentsOf(root))
        {
          m_groups.merge(root, boundary.neighbour);
        }
      }
    }
  }
};

struct Room
{
  size_t id;
  /** In ascending order. */
  std::vector<size_t> cells;
  vm::bbox3d bounds;
  double volume;
  mdl::BspLeafContents contents;
  /** The point entities whose origin lies in one of the room's cells, in file order. */
  std::vector<const mdl::EntityNode*> entities;
};

struct RoomGraph
{
  std::vector<Room> rooms;
  /** Indexed by cell. */
  std::vector<std::optional<size_t>> roomOfCell;
  /** Indexed by cell: the point entities whose origin lies in it, in file order. */
  std::vector<std::vector<const mdl::EntityNode*>> entitiesOfCell;
};

RoomGraph buildRooms(
  const CellGraph& graph,
  const mdl::BspLeafTree& tree,
  const mdl::WorldNode& worldNode,
  const double splitRatio)
{
  auto groups = RoomMerger{graph, splitRatio}.merge();

  auto result = RoomGraph{};
  result.roomOfCell.resize(graph.cells.size());
  result.entitiesOfCell.resize(graph.cells.size());

  // the cells are visited in order, so rooms are numbered by their lowest cell
  auto roomOfRoot = std::map<size_t, size_t>{};
  for (size_t i = 0; i < graph.cells.size(); ++i)
  {
    if (!graph.cells[i])
    {
      continue;
    }
    const auto root = groups.find(i);
    const auto [it, inserted] = roomOfRoot.try_emplace(root, result.rooms.size());
    if (inserted)
    {
      auto cells = groups.cells(root);
      std::ranges::sort(cells);
      const auto volume = std::accumulate(
        cells.begin(), cells.end(), 0.0, [&](const double sum, const size_t cell) {
          return sum + graph.cells[cell]->bounds.volume();
        });
      result.rooms.push_back(
        {result.rooms.size(),
         cells,
         boundsOf(graph, cells),
         volume,
         graph.cells[i]->contents,
         {}});
    }
    result.roomOfCell[i] = it->second;
  }

  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (entityNode->hasChildren())
    {
      // a brush entity
      continue;
    }
    // BSP leaf i + 1 is vis leaf i, and leaf 0 is solid
    const auto leaf = tree.findLeaf(entityNode->entity().origin());
    if (leaf == 0 || leaf > graph.cells.size())
    {
      continue;
    }
    const auto cell = leaf - 1;
    if (const auto room = result.roomOfCell[cell])
    {
      result.entitiesOfCell[cell].push_back(entityNode);
      result.rooms[*room].entities.push_back(entityNode);
    }
  }
  return result;
}

bool isWithin(const std::optional<vm::bbox3d>& within, const vm::bbox3d& bounds)
{
  return !within || within->expand(SpatialTolerance).contains(bounds);
}

Json pathsJson(
  const std::vector<const mdl::EntityNode*>& entities,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::array();
  for (const auto* entity : entities)
  {
    result.push(paths.at(entity));
  }
  return result;
}

Json roomToJson(
  const Room& room,
  const std::vector<std::string>& keyClasses,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto keyEntities = Json::array();
  for (const auto* entity : room.entities)
  {
    if (isKeyEntity(entity->entity(), keyClasses))
    {
      keyEntities.push(paths.at(entity));
    }
  }

  auto result = Json::object();
  result.set("id", room.id);
  result.set("bounds", toJson(room.bounds));
  result.set("volume", room.volume);
  result.set("leaves", room.cells.size());
  result.set("contents", toString(room.contents));
  result.set("entities", pathsJson(room.entities, paths));
  result.set("key_entities", std::move(keyEntities));
  result.set("openings", Json::array());
  return result;
}

void setRooms(
  Json& result,
  const RoomGraph& roomGraph,
  const Config& config,
  const RoomsOptions& options,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto listed = std::vector<const Room*>{};
  for (const auto& room : roomGraph.rooms)
  {
    if (
      (!options.room || room.id == *options.room)
      && isWithin(options.within, room.bounds))
    {
      listed.push_back(&room);
    }
  }

  const auto& keyClasses = keyEntityClasses(config.keyEntities);
  result.set("rooms", page(listed, options.paging, [&](const Room* room) {
               return roomToJson(*room, keyClasses, paths);
             }));
}

/**
 * Why a portal between two cells of different rooms is cut: `liquid_surface` where the
 * contents change, as the opening's kind will say, else `constriction`; null if the
 * portal is not cut.
 */
Json cutOf(const CellGraph& graph, const RoomGraph& roomGraph, const Portal& portal)
{
  if (roomGraph.roomOfCell[portal.a] == roomGraph.roomOfCell[portal.b])
  {
    return nullptr;
  }
  return graph.cells[portal.a]->contents != graph.cells[portal.b]->contents
           ? "liquid_surface"
           : "constriction";
}

void setCells(
  Json& result,
  const CellGraph& graph,
  const RoomGraph& roomGraph,
  const RoomsOptions& options,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto listed = std::vector<size_t>{};
  for (size_t i = 0; i < graph.cells.size(); ++i)
  {
    if (
      graph.cells[i] && (!options.room || roomGraph.roomOfCell[i] == *options.room)
      && isWithin(options.within, graph.cells[i]->bounds))
    {
      listed.push_back(i);
    }
  }

  const auto cells = page(listed, options.paging, [&](const size_t i) {
    const auto& cell = *graph.cells[i];
    auto json = Json::object();
    json.set("id", i);
    json.set("bounds", toJson(cell.bounds));
    json.set("volume", cell.bounds.volume());
    json.set("contents", toString(cell.contents));
    json.set("room", *roomGraph.roomOfCell[i]);
    json.set("entities", pathsJson(roomGraph.entitiesOfCell[i], paths));
    return json;
  });

  // the portals of the listed cells
  auto isListed = std::vector<bool>(graph.cells.size(), false);
  for (const auto& item : cells["items"].asArray())
  {
    isListed[size_t(item["id"].asNumber())] = true;
  }
  auto portals = Json::array();
  for (const auto& portal : graph.portals)
  {
    if (isListed[portal.a] || isListed[portal.b])
    {
      auto json = Json::object();
      json.set("cells", Json::Array{portal.a, portal.b});
      json.set("center", toJson(portal.center));
      json.set("normal", toJson(graph.planes[portal.plane].normal));
      json.set("area", portal.area);
      json.set("cut", cutOf(graph, roomGraph, portal));
      portals.push(std::move(json));
    }
  }

  result.set("cells", cells);
  result.set("portals", std::move(portals));
}

} // namespace

Json rooms(LoadedMap& loadedMap, const Config& config, const RoomsOptions& options)
{
  const auto compiled = options.fromArtifacts
                          ? readCompiledMap(
                              *options.fromArtifacts,
                              mapName(loadedMap),
                              loadedMap.rev,
                              ErrorCode::StaleArtifact)
                          : compileForRooms(loadedMap, config, options);

  const auto& worldNode = loadedMap.map->worldNode();
  const auto graph = buildCellGraph(compiled);
  const auto roomGraph = buildRooms(graph, compiled.tree, worldNode, options.splitRatio);
  const auto paths = nodePaths(worldNode);

  auto method = Json::object();
  method.set("name", "constriction");
  method.set("split_ratio", options.splitRatio);

  auto result = Json::object();
  result.set("schema", "tbcli.rooms/1");
  result.set("rev", loadedMap.rev);
  result.set("heuristic", true);
  result.set("method", std::move(method));
  if (options.cells)
  {
    setCells(result, graph, roomGraph, options, paths);
  }
  else
  {
    setRooms(result, roomGraph, config, options, paths);
  }
  return result;
}

} // namespace tb::cli
