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
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/LoadBspLeafTree.h"
#include "mdl/Map.h"
#include "mdl/PortalFile.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"

#include "vm/vec.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
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
 * How far apart the unit normals of two portals' planes may be for the portals to count
 * as coplanar when they are merged into one opening.
 */
constexpr auto CoplanarNormalEpsilon = 1e-3;

/** How far from vertical a plane's normal may be for the plane to count as horizontal. */
constexpr auto HorizontalPlaneEpsilon = 1e-6;

const auto TeleportTriggerClassname = std::string{"trigger_teleport"};
const auto TeleportDestinationClassname = std::string{"info_teleport_destination"};

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
  std::vector<vm::vec3d> polygon;
};

/** A line segment. */
struct Segment
{
  vm::vec3d start;
  vm::vec3d end;
};

std::vector<Segment> edgesOf(const std::vector<vm::vec3d>& polygon)
{
  auto result = std::vector<Segment>{};
  for (size_t i = 0; i < polygon.size(); ++i)
  {
    result.push_back({polygon[i], polygon[(i + 1) % polygon.size()]});
  }
  return result;
}

/** The area of a polygon and its unit normal, by Newell's method. */
std::pair<double, vm::vec3d> areaAndNormal(const std::vector<vm::vec3d>& vertices)
{
  auto sum = vm::vec3d{0, 0, 0};
  for (const auto& [current, next] : edgesOf(vertices))
  {
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
    auto polygon = portal.polygon.vertices()
                   | std::views::transform([](const auto& v) { return vm::vec3d{v}; })
                   | kdl::ranges::to<std::vector>();
    const auto [area, normal] = areaAndNormal(polygon);
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
    result.portals.push_back(
      {portal.a, portal.b, plane, sideA, sideB, area, center, std::move(polygon)});

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
  /** The ids of the room's openings, in ascending order. */
  std::vector<size_t> openings;
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
         {},
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

/**
 * The part of `segment` that `other` covers, as an interval of parameters of `segment`
 * from 0 to 1, if `other` lies on the same line and they share more than a point.
 */
std::optional<std::pair<double, double>> sharedPart(
  const Segment& segment, const Segment& other)
{
  const auto direction = segment.end - segment.start;
  const auto lengthSquared = vm::squared_length(direction);
  if (lengthSquared == 0.0)
  {
    return std::nullopt;
  }
  // the parameter of a point's projection onto the line, and its distance from it
  const auto project = [&](const vm::vec3d& point) {
    const auto t = vm::dot(point - segment.start, direction) / lengthSquared;
    return std::pair{t, vm::length(point - (segment.start + t * direction))};
  };
  const auto [t0, distance0] = project(other.start);
  const auto [t1, distance1] = project(other.end);
  if (distance0 > SpatialTolerance || distance1 > SpatialTolerance)
  {
    return std::nullopt;
  }
  const auto from = std::max(0.0, std::min(t0, t1));
  const auto to = std::min(1.0, std::max(t0, t1));
  if ((to - from) * std::sqrt(lengthSquared) <= SpatialTolerance)
  {
    return std::nullopt;
  }
  return std::pair{from, to};
}

/** Whether two polygons share part of an edge. */
bool shareEdge(const std::vector<vm::vec3d>& lhs, const std::vector<vm::vec3d>& rhs)
{
  const auto rhsEdges = edgesOf(rhs);
  return std::ranges::any_of(edgesOf(lhs), [&](const Segment& edge) {
    return std::ranges::any_of(rhsEdges, [&](const Segment& other) {
      return sharedPart(edge, other).has_value();
    });
  });
}

/**
 * The boundary of a set of coplanar polygons that share edges: their edges, less the
 * parts that two of them share.
 */
std::vector<Segment> boundaryOf(
  const std::vector<const std::vector<vm::vec3d>*>& polygons)
{
  auto result = std::vector<Segment>{};
  for (size_t i = 0; i < polygons.size(); ++i)
  {
    for (const auto& edge : edgesOf(*polygons[i]))
    {
      auto shared = std::vector<std::pair<double, double>>{};
      for (size_t j = 0; j < polygons.size(); ++j)
      {
        if (i == j)
        {
          continue;
        }
        for (const auto& other : edgesOf(*polygons[j]))
        {
          if (const auto part = sharedPart(edge, other))
          {
            shared.push_back(*part);
          }
        }
      }
      std::ranges::sort(shared);

      const auto pointAt = [&](const double t) {
        return edge.start + t * (edge.end - edge.start);
      };
      const auto length = vm::length(edge.end - edge.start);
      auto from = 0.0;
      for (const auto& [sharedFrom, sharedTo] : shared)
      {
        if ((sharedFrom - from) * length > SpatialTolerance)
        {
          result.push_back({pointAt(from), pointAt(sharedFrom)});
        }
        from = std::max(from, sharedTo);
      }
      if ((1.0 - from) * length > SpatialTolerance)
      {
        result.push_back({pointAt(from), edge.end});
      }
    }
  }
  return result;
}

/** Whether the segment intersects the box. */
bool intersects(const vm::bbox3d& box, const Segment& segment)
{
  const auto direction = segment.end - segment.start;
  auto from = 0.0;
  auto to = 1.0;
  for (size_t axis = 0; axis < 3; ++axis)
  {
    if (direction[axis] == 0.0)
    {
      if (segment.start[axis] < box.min[axis] || segment.start[axis] > box.max[axis])
      {
        return false;
      }
      continue;
    }
    auto t0 = (box.min[axis] - segment.start[axis]) / direction[axis];
    auto t1 = (box.max[axis] - segment.start[axis]) / direction[axis];
    if (t0 > t1)
    {
      std::swap(t0, t1);
    }
    from = std::max(from, t0);
    to = std::min(to, t1);
    if (from > to)
    {
      return false;
    }
  }
  return true;
}

/**
 * The part of a convex polygon on one side of an axial plane: where the coordinate
 * along the axis is at least `value` if `keepAbove`, else at most `value`.
 */
std::vector<vm::vec3d> clip(
  const std::vector<vm::vec3d>& polygon,
  const size_t axis,
  const double value,
  const bool keepAbove)
{
  const auto inside = [&](const vm::vec3d& point) {
    return keepAbove ? point[axis] >= value : point[axis] <= value;
  };
  auto result = std::vector<vm::vec3d>{};
  for (const auto& [current, next] : edgesOf(polygon))
  {
    if (inside(current))
    {
      result.push_back(current);
    }
    if (inside(current) != inside(next))
    {
      const auto t = (value - current[axis]) / (next[axis] - current[axis]);
      result.push_back(current + t * (next - current));
    }
  }
  return result;
}

/**
 * Whether a convex polygon overlaps a box: the part of it inside the box has an area.
 * Along an axis on which the polygon is flat, as an axial portal is, it only needs to lie
 * within 0.01 of the box, so that a door flush with an opening overlaps it.
 */
bool overlaps(const vm::bbox3d& box, std::vector<vm::vec3d> polygon)
{
  for (size_t axis = 0; axis < 3; ++axis)
  {
    const auto [min, max] = std::ranges::minmax(
      polygon | std::views::transform([&](const auto& point) { return point[axis]; }));
    if (max - min <= SpatialTolerance)
    {
      if (
        max < box.min[axis] - SpatialTolerance || min > box.max[axis] + SpatialTolerance)
      {
        return false;
      }
      continue;
    }
    polygon = clip(polygon, axis, box.min[axis], true);
    polygon = clip(polygon, axis, box.max[axis], false);
    if (polygon.size() < 3)
    {
      return false;
    }
  }
  return areaAndNormal(polygon).first > MinPortalArea;
}

/**
 * Two perpendicular unit directions in a plane with the given unit normal: the first
 * horizontal and the second along the plane's slope, or x and y if the plane is
 * horizontal.
 */
std::pair<vm::vec3d, vm::vec3d> inPlaneAxes(const vm::vec3d& normal)
{
  const auto horizontal = vm::cross(vm::vec3d{0, 0, 1}, normal);
  if (vm::length(horizontal) < HorizontalPlaneEpsilon)
  {
    return {vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};
  }
  const auto u = vm::normalize(horizontal);
  return {u, vm::cross(normal, u)};
}

/** The extent of the points of the given polygons along the given unit direction. */
double extentAlong(
  const std::vector<const std::vector<vm::vec3d>*>& polygons, const vm::vec3d& direction)
{
  auto min = std::numeric_limits<double>::max();
  auto max = std::numeric_limits<double>::lowest();
  for (const auto* polygon : polygons)
  {
    for (const auto& point : *polygon)
    {
      const auto distance = vm::dot(point, direction);
      min = std::min(min, distance);
      max = std::max(max, distance);
    }
  }
  return max - min;
}

/** The centroid of a convex polygon, by a fan of triangles. */
vm::vec3d centroidOf(const std::vector<vm::vec3d>& polygon)
{
  auto sum = vm::vec3d{0, 0, 0};
  auto totalArea = 0.0;
  for (size_t i = 1; i + 1 < polygon.size(); ++i)
  {
    const auto area =
      vm::length(vm::cross(polygon[i] - polygon[0], polygon[i + 1] - polygon[0])) / 2.0;
    sum = sum + area * (polygon[0] + polygon[i] + polygon[i + 1]) / 3.0;
    totalArea += area;
  }
  return totalArea > 0.0 ? sum / totalArea : polygon.front();
}

/**
 * Whether qbsp builds the brushes of an entity of the given class into the world: those
 * of worldspawn and func_group, and for rooms' `-nodetail` run, those of func_detail and
 * its variants. This differs from Leak.cpp's isSealing, which judges verify's qbsp run
 * without `-nodetail`, where func_detail_wall, func_detail_fence and
 * func_detail_illusionary do not seal.
 */
bool isWorldClass(const std::string& classname)
{
  return classname == mdl::EntityPropertyValues::WorldspawnClassname
         || classname == mdl::EntityPropertyValues::GroupClassname
         || classname.starts_with("func_detail");
}

/** The coplanar merge of the cut portals between two rooms. */
struct Opening
{
  size_t id;
  /** Whether the contents change across it. */
  bool liquidSurface;
  /** Ascending. */
  std::array<size_t, 2> rooms;
  /** In file order. */
  std::vector<const Portal*> portals;
  /** Points from the first room into the second. */
  vm::vec3d normal;
  vm::vec3d center;
  vm::bbox3d bounds;
  double area;
  double width;
  double height;
  /** The edges of the portals, less the parts that two of them share. */
  std::vector<Segment> boundary;
};

/** Disjoint sets of the indices 0 to n - 1, each named by its lowest index. */
class DisjointSets
{
private:
  std::vector<size_t> m_parent;
  size_t m_count;

public:
  explicit DisjointSets(const size_t size)
    : m_parent(size)
    , m_count{size}
  {
    std::iota(m_parent.begin(), m_parent.end(), size_t{0});
  }

  size_t find(size_t i)
  {
    while (m_parent[i] != i)
    {
      m_parent[i] = m_parent[m_parent[i]];
      i = m_parent[i];
    }
    return i;
  }

  /** Joins the sets of the given indices; returns whether they were apart. */
  bool join(const size_t lhs, const size_t rhs)
  {
    const auto lhsRoot = find(lhs);
    const auto rhsRoot = find(rhs);
    if (lhsRoot == rhsRoot)
    {
      return false;
    }
    m_parent[std::max(lhsRoot, rhsRoot)] = std::min(lhsRoot, rhsRoot);
    --m_count;
    return true;
  }

  /** The number of sets. */
  size_t count() const { return m_count; }
};

/** A cut portal between two rooms, with its plane oriented from the first room. */
struct CutPortal
{
  const Portal* portal;
  /** Points from the first room into the second. */
  vm::vec3d normal;
  /** The distance of the plane from the origin along `normal`. */
  double distance;
};

CutPortal cutPortal(
  const CellGraph& graph,
  const RoomGraph& roomGraph,
  const size_t firstRoom,
  const Portal& portal)
{
  // the plane's normal points to its front side
  const auto sideOfFirstRoom =
    roomGraph.roomOfCell[portal.a] == firstRoom ? portal.sideA : portal.sideB;
  const auto& plane = graph.planes[portal.plane];
  return sideOfFirstRoom == Side::Front
           ? CutPortal{&portal, -plane.normal, -plane.distance}
           : CutPortal{&portal, plane.normal, plane.distance};
}

/**
 * Whether two cut portals lie in one plane, within tolerances, and face the same way, so
 * that a gap whose portals' planes were told apart by rounding is still one opening.
 */
bool isCoplanar(const CutPortal& lhs, const CutPortal& rhs)
{
  return vm::dot(lhs.normal, rhs.normal) > 1.0 - CoplanarNormalEpsilon
         && std::abs(lhs.distance - rhs.distance) <= PlaneDistanceEpsilon;
}

/**
 * The given cut portals grouped into openings: those that are coplanar and share an
 * edge, directly or through others. The groups are ordered by their first portal.
 */
std::vector<std::vector<CutPortal>> mergeCoplanar(const std::vector<CutPortal>& portals)
{
  auto sets = DisjointSets{portals.size()};
  for (size_t i = 0; i < portals.size(); ++i)
  {
    for (size_t j = i + 1; j < portals.size(); ++j)
    {
      if (
        sets.find(i) != sets.find(j) && isCoplanar(portals[i], portals[j])
        && shareEdge(portals[i].portal->polygon, portals[j].portal->polygon))
      {
        sets.join(i, j);
      }
    }
  }

  auto groups = std::map<size_t, std::vector<CutPortal>>{};
  for (size_t i = 0; i < portals.size(); ++i)
  {
    groups[sets.find(i)].push_back(portals[i]);
  }
  return groups | std::views::values | kdl::ranges::to<std::vector>();
}

Opening makeOpening(
  const CellGraph& graph,
  const std::array<size_t, 2>& rooms,
  const std::vector<CutPortal>& cutPortals)
{
  const auto& first = *cutPortals.front().portal;
  const auto& normal = cutPortals.front().normal;
  auto portals = cutPortals | std::views::transform(&CutPortal::portal)
                 | kdl::ranges::to<std::vector>();

  auto polygons = std::vector<const std::vector<vm::vec3d>*>{};
  auto weightedCenter = vm::vec3d{0, 0, 0};
  auto area = 0.0;
  auto bounds = vm::bbox3d{first.polygon.front(), first.polygon.front()};
  for (const auto* portal : portals)
  {
    polygons.push_back(&portal->polygon);
    weightedCenter = weightedCenter + portal->area * centroidOf(portal->polygon);
    area += portal->area;
    for (const auto& point : portal->polygon)
    {
      bounds = vm::merge(bounds, point);
    }
  }
  const auto [u, v] = inPlaneAxes(normal);

  return {
    0,
    graph.cells[first.a]->contents != graph.cells[first.b]->contents,
    rooms,
    std::move(portals),
    normal,
    weightedCenter / area,
    bounds,
    area,
    extentAlong(polygons, u),
    extentAlong(polygons, v),
    boundaryOf(polygons),
  };
}

/**
 * The openings between the rooms of the given room graph, which also gives each room the
 * ids of its openings. Openings are numbered by their rooms, then by their first portal.
 */
std::vector<Opening> buildOpenings(const CellGraph& graph, RoomGraph& roomGraph)
{
  // the cut portals between each pair of rooms, in file order
  auto cutPortals = std::map<std::array<size_t, 2>, std::vector<CutPortal>>{};
  for (const auto& portal : graph.portals)
  {
    const auto roomA = *roomGraph.roomOfCell[portal.a];
    const auto roomB = *roomGraph.roomOfCell[portal.b];
    if (roomA != roomB)
    {
      const auto rooms = std::array{std::min(roomA, roomB), std::max(roomA, roomB)};
      cutPortals[rooms].push_back(cutPortal(graph, roomGraph, rooms[0], portal));
    }
  }

  auto result = std::vector<Opening>{};
  for (const auto& [rooms, portals] : cutPortals)
  {
    for (const auto& group : mergeCoplanar(portals))
    {
      result.push_back(makeOpening(graph, rooms, group));
    }
  }

  // the graph's portals are in file order
  std::ranges::sort(result, [](const Opening& lhs, const Opening& rhs) {
    return std::tie(lhs.rooms, lhs.portals.front())
           < std::tie(rhs.rooms, rhs.portals.front());
  });
  for (size_t i = 0; i < result.size(); ++i)
  {
    result[i].id = i;
    for (const auto room : result[i].rooms)
    {
      roomGraph.rooms[room].openings.push_back(i);
    }
  }
  return result;
}

/**
 * The number of connected components of the rooms, joined by their openings and by
 * teleporters: a trigger_teleport joins the rooms of the cells its bounds overlap to the
 * room of each info_teleport_destination whose targetname it targets.
 */
size_t countComponents(
  const CellGraph& graph,
  const RoomGraph& roomGraph,
  const std::vector<Opening>& openings,
  const mdl::WorldNode& worldNode)
{
  auto components = DisjointSets{roomGraph.rooms.size()};
  for (const auto& opening : openings)
  {
    components.join(opening.rooms[0], opening.rooms[1]);
  }

  auto roomsOfDestinations = std::multimap<std::string, size_t>{};
  for (const auto& room : roomGraph.rooms)
  {
    for (const auto* entityNode : room.entities)
    {
      const auto& entity = entityNode->entity();
      if (entity.classname() == TeleportDestinationClassname)
      {
        if (const auto* name = entity.property(mdl::EntityPropertyKeys::Targetname))
        {
          roomsOfDestinations.emplace(*name, room.id);
        }
      }
    }
  }

  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    const auto& entity = entityNode->entity();
    const auto* target = entity.property(mdl::EntityPropertyKeys::Target);
    if (entity.classname() != TeleportTriggerClassname || !target)
    {
      continue;
    }
    const auto [destinationsBegin, destinationsEnd] =
      roomsOfDestinations.equal_range(*target);
    const auto& bounds = entityNode->logicalBounds();
    for (size_t cell = 0; cell < graph.cells.size(); ++cell)
    {
      // the bounds share a volume, not only a face
      if (
        graph.cells[cell]
        && vm::intersect(bounds, graph.cells[cell]->bounds).volume() > 0.0)
      {
        for (auto it = destinationsBegin; it != destinationsEnd; ++it)
        {
          components.join(*roomGraph.roomOfCell[cell], it->second);
        }
      }
    }
  }
  return components.count();
}

bool isWithin(const std::optional<vm::bbox3d>& within, const vm::bbox3d& bounds)
{
  return !within || within->expand(SpatialTolerance).contains(bounds);
}

template <typename NodeType>
Json pathsJson(
  const std::vector<const NodeType*>& nodes,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::array();
  for (const auto* node : nodes)
  {
    result.push(paths.at(node));
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
  result.set("openings", Json::Array(room.openings.begin(), room.openings.end()));
  return result;
}

/** The brushes and brush entities that frame and gate openings. */
struct OpeningSurroundings
{
  /** The brushes that qbsp builds into the world, in file order. */
  std::vector<const mdl::BrushNode*> worldBrushes;
  /** The other brush entities, such as doors, in file order. */
  std::vector<const mdl::EntityNode*> brushEntities;
};

OpeningSurroundings collectSurroundings(const mdl::WorldNode& worldNode)
{
  auto result = OpeningSurroundings{};
  const auto contents = collectContents(worldNode);
  for (const auto* brushNode : contents.brushes)
  {
    if (
      const auto* entityNode = brushNode->entity();
      entityNode && isWorldClass(entityNode->entity().classname()))
    {
      result.worldBrushes.push_back(brushNode);
    }
  }
  for (const auto* entityNode : contents.entities)
  {
    if (entityNode->hasChildren() && !isWorldClass(entityNode->entity().classname()))
    {
      result.brushEntities.push_back(entityNode);
    }
  }
  return result;
}

/** The brushes that frame an opening and the brush entities that gate it. */
struct OpeningFrame
{
  /** The world brushes whose bounds touch the opening's boundary, in file order. */
  std::vector<const mdl::BrushNode*> brushes;
  /** The other brush entities whose bounds overlap the opening, in file order. */
  std::vector<const mdl::EntityNode*> gatedBy;
};

OpeningFrame frameOf(const Opening& opening, const OpeningSurroundings& surroundings)
{
  auto brushes = std::vector<const mdl::BrushNode*>{};
  for (const auto* brushNode : surroundings.worldBrushes)
  {
    const auto bounds = brushNode->logicalBounds().expand(SpatialTolerance);
    if (std::ranges::any_of(opening.boundary, [&](const Segment& segment) {
          return intersects(bounds, segment);
        }))
    {
      brushes.push_back(brushNode);
    }
  }

  auto gatedBy = std::vector<const mdl::EntityNode*>{};
  for (const auto* entityNode : surroundings.brushEntities)
  {
    if (std::ranges::any_of(opening.portals, [&](const Portal* portal) {
          return overlaps(entityNode->logicalBounds(), portal->polygon);
        }))
    {
      gatedBy.push_back(entityNode);
    }
  }
  return {std::move(brushes), std::move(gatedBy)};
}

Json openingToJson(
  const Opening& opening,
  const OpeningFrame& frame,
  const bool raw,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::object();
  result.set("id", opening.id);
  if (opening.liquidSurface)
  {
    result.set("kind", "liquid_surface");
  }
  result.set("rooms", Json::Array{opening.rooms[0], opening.rooms[1]});
  result.set("center", toJson(opening.center));
  result.set("normal", toJson(opening.normal));
  result.set("bounds", toJson(opening.bounds));
  result.set("area", opening.area);
  result.set("width", opening.width);
  result.set("height", opening.height);
  result.set("brushes", pathsJson(frame.brushes, paths));
  result.set("gated_by", pathsJson(frame.gatedBy, paths));
  if (raw)
  {
    auto polygons = Json::array();
    for (const auto* portal : opening.portals)
    {
      auto polygon = Json::array();
      for (const auto& point : portal->polygon)
      {
        polygon.push(toJson(point));
      }
      polygons.push(std::move(polygon));
    }
    result.set("polygons", std::move(polygons));
  }
  return result;
}

void setRooms(
  Json& result,
  const RoomGraph& roomGraph,
  const std::vector<Opening>& openings,
  const OpeningSurroundings& surroundings,
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

  // the openings of the rooms on the page, in id order
  auto isOnPage = std::vector<bool>(roomGraph.rooms.size(), false);
  for (const auto* room : listed | std::views::drop(options.paging.offset)
                            | std::views::take(options.paging.limit))
  {
    isOnPage[room->id] = true;
  }
  auto openingsJson = Json::array();
  for (const auto& opening : openings)
  {
    if (isOnPage[opening.rooms[0]] || isOnPage[opening.rooms[1]])
    {
      openingsJson.push(
        openingToJson(opening, frameOf(opening, surroundings), options.raw, paths));
    }
  }

  const auto& keyClasses = keyEntityClasses(config.keyEntities);
  result.set("rooms", page(listed, options.paging, [&](const Room* room) {
               return roomToJson(*room, keyClasses, paths);
             }));
  result.set("openings", std::move(openingsJson));
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
  auto roomGraph = buildRooms(graph, compiled.tree, worldNode, options.splitRatio);
  const auto openings = buildOpenings(graph, roomGraph);
  const auto paths = nodePaths(worldNode);

  auto method = Json::object();
  method.set("name", "constriction");
  method.set("split_ratio", options.splitRatio);

  auto result = Json::object();
  result.set("schema", "tbcli.rooms/1");
  result.set("rev", loadedMap.rev);
  result.set("heuristic", true);
  result.set("method", std::move(method));

  auto summary = Json::object();
  summary.set("rooms", roomGraph.rooms.size());
  summary.set("openings", openings.size());
  summary.set("components", countComponents(graph, roomGraph, openings, worldNode));
  result.set("summary", std::move(summary));

  if (options.cells)
  {
    setCells(result, graph, roomGraph, options, paths);
  }
  else
  {
    setRooms(
      result,
      roomGraph,
      openings,
      collectSurroundings(worldNode),
      config,
      options,
      paths);
  }
  return result;
}

} // namespace tb::cli
