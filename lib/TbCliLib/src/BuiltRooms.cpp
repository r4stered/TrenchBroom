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

#include "cli/BuiltRooms.h"

#include "base/Result.h"
#include "cli/CliError.h"
#include "cli/EditOps.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/NodeJson.h"
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Sha256.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LinkedGroupUtils.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeContents.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/ranges/contains.h"
#include "kd/ranges/to.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>

namespace tb::cli
{
namespace
{

namespace Keys
{
/** The prefix of every key that tbcli keeps on a carrier. */
constexpr auto Prefix = std::string_view{"_tbcli_"};
const auto Kind = std::string{"_tbcli_kind"};
const auto Name = std::string{"_tbcli_name"};
const auto Size = std::string{"_tbcli_size"};
const auto Wall = std::string{"_tbcli_wall"};
const auto Place = std::string{"_tbcli_place"};
const auto From = std::string{"_tbcli_from"};
const auto To = std::string{"_tbcli_to"};
const auto Width = std::string{"_tbcli_width"};
const auto Height = std::string{"_tbcli_height"};
const auto Offset = std::string{"_tbcli_offset"};
const auto Fingerprint = std::string{"_tbcli_fp"};
} // namespace Keys

/** The `_tbcli_kind` of a built room's carrier. */
const auto RoomKind = std::string{"room"};

/** The `_tbcli_kind` of a connection's carrier. */
const auto ConnectionKind = std::string{"connection"};

/** The prefix of the name of the group that wraps a built room's carrier. */
constexpr auto RoomGroupPrefix = std::string_view{"built_room:"};

/** The prefix of the name of the group that wraps a connection's carrier. */
constexpr auto ConnectionGroupPrefix = std::string_view{"connection:"};

constexpr auto RelationNames =
  std::array<std::string_view, 4>{"east_of", "west_of", "north_of", "south_of"};

/** The number of hex digits of a geometry fingerprint. */
constexpr auto FingerprintLength = size_t{16};

/** How far two rooms' walls may overlap before they count as overlapping. */
constexpr auto OverlapTolerance = 0.001;

std::optional<Relation> parseRelation(const std::string_view name)
{
  const auto it = std::ranges::find(RelationNames, name);
  return it != RelationNames.end()
           ? std::optional{static_cast<Relation>(it - RelationNames.begin())}
           : std::nullopt;
}

std::string_view toString(const Relation relation)
{
  return RelationNames[static_cast<size_t>(relation)];
}

bool isRoomName(const std::string_view name)
{
  return !name.empty() && std::ranges::all_of(name, isWordChar);
}

/** The relation and the room of a well-formed placement. */
std::pair<Relation, std::string> relationOf(const Json& place)
{
  for (const auto& member : place.asObject())
  {
    if (const auto relation = parseRelation(member.key))
    {
      return {*relation, member.value.asString()};
    }
  }
  contract_assert(false);
  return {Relation::EastOf, ""};
}

Placement placementFromJson(const Json& place)
{
  auto [relation, room] = relationOf(place);
  return Placement{relation, std::move(room), place["gap"].asNumber()};
}

/** `east_of A 64`, as `_tbcli_place` stores it. */
std::string placementToString(const Placement& place)
{
  return fmt::format(
    "{} {} {}", toString(place.relation), place.room, formatNumber(place.gap));
}

std::optional<Placement> parsePlacement(const std::string& str)
{
  const auto parts = kdl::str_split(str, " ");
  if (parts.size() != 3)
  {
    return std::nullopt;
  }
  const auto relation = parseRelation(parts[0]);
  const auto gap = parseDecimal(parts[2]);
  return relation && isRoomName(parts[1]) && gap
           ? std::optional{Placement{*relation, parts[1], *gap}}
           : std::nullopt;
}

/** The parameters that a carrier's keys store, or nothing if a key is malformed. */
std::optional<RoomParams> parseRoomParams(const mdl::Entity& entity)
{
  const auto* sizeValue = entity.property(Keys::Size);
  const auto* wallValue = entity.property(Keys::Wall);
  if (!sizeValue || !wallValue)
  {
    return std::nullopt;
  }

  const auto size = parseNumberList(*sizeValue);
  const auto wall = parseDecimal(*wallValue);
  if (
    !size || size->size() != 3
    || std::ranges::any_of(*size, [](auto s) { return s <= 0; }) || !wall || *wall <= 0)
  {
    return std::nullopt;
  }

  auto result = RoomParams{vm::vec3d{(*size)[0], (*size)[1], (*size)[2]}, *wall, {}};
  if (const auto* placeValue = entity.property(Keys::Place))
  {
    result.place = parsePlacement(*placeValue);
    if (!result.place)
    {
      return std::nullopt;
    }
  }
  return result;
}

/**
 * Whether the entity is a carrier of the given kind: a plain func_group, not one of the
 * editor's own groups, whose `_tbcli_kind` is that kind.
 */
bool isCarrierOfKind(const mdl::EntityNode& entityNode, const std::string& kind)
{
  const auto& entity = entityNode.entity();
  return entity.classname() == mdl::EntityPropertyValues::GroupClassname
         && !entity.hasProperty(mdl::EntityPropertyKeys::TbGroupType)
         && entity.hasProperty(Keys::Kind, kind);
}

/** Whether the entity is a built room's carrier. */
bool isRoomCarrier(const mdl::EntityNode& entityNode)
{
  return isCarrierOfKind(entityNode, RoomKind)
         && entityNode.entity().property(Keys::Name);
}

/** Whether the entity is a connection's carrier. */
bool isConnectionCarrier(const mdl::EntityNode& entityNode)
{
  return isCarrierOfKind(entityNode, ConnectionKind);
}

/** The walls, floor and ceiling around an interior, as in the reference lowering. */
std::vector<vm::bbox3d> shellSlabs(const vm::bbox3d& interior, const double wall)
{
  const auto x0 = interior.min.x(), y0 = interior.min.y(), z0 = interior.min.z();
  const auto x1 = interior.max.x(), y1 = interior.max.y(), z1 = interior.max.z();
  const auto t = wall;
  const auto box = [](const auto... coords) {
    const auto c = std::array{coords...};
    return vm::bbox3d{vm::vec3d{c[0], c[1], c[2]}, vm::vec3d{c[3], c[4], c[5]}};
  };
  return {
    // floor and ceiling
    box(x0 - t, y0 - t, z0 - t, x1 + t, y1 + t, z0),
    box(x0 - t, y0 - t, z1, x1 + t, y1 + t, z1 + t),
    // west and east walls, the full depth of the room
    box(x0 - t, y0 - t, z0, x0, y1 + t, z1),
    box(x1, y0 - t, z0, x1 + t, y1 + t, z1),
    // south and north walls, between the west and east walls
    box(x0, y0 - t, z0, x1, y0, z1),
    box(x0, y1, z0, x1, y1 + t, z1),
  };
}

bool hasVolume(const vm::bbox3d& box)
{
  return box.min.x() < box.max.x() && box.min.y() < box.max.y()
         && box.min.z() < box.max.z();
}

/** Whether two boxes share a volume; boxes that only touch do not. */
bool overlaps(const vm::bbox3d& a, const vm::bbox3d& b, const double tolerance = 0.0)
{
  for (size_t k = 0; k < 3; ++k)
  {
    if (!(a.max[k] > b.min[k] + tolerance && b.max[k] > a.min[k] + tolerance))
    {
      return false;
    }
  }
  return true;
}

/** The pieces of box a that lie outside box b. */
std::vector<vm::bbox3d> subtract(const vm::bbox3d& a, const vm::bbox3d& b)
{
  if (!overlaps(a, b))
  {
    return {a};
  }

  auto result = std::vector<vm::bbox3d>{};
  auto rest = a;
  for (size_t k = 0; k < 3; ++k)
  {
    if (rest.min[k] < b.min[k])
    {
      auto piece = rest;
      piece.max[k] = b.min[k];
      result.push_back(piece);
      rest.min[k] = b.min[k];
    }
    if (rest.max[k] > b.max[k])
    {
      auto piece = rest;
      piece.min[k] = b.max[k];
      result.push_back(piece);
      rest.max[k] = b.max[k];
    }
  }
  return result;
}

/**
 * The boxes that slabs lower to, as in the reference lowering: the slabs less the given
 * interiors of every room and corridor, so that no slab reaches into one.
 */
std::vector<vm::bbox3d> lowerSlabs(
  const std::vector<vm::bbox3d>& slabs, const std::vector<vm::bbox3d>& interiors)
{
  auto result = std::vector<vm::bbox3d>{};
  for (const auto& slab : slabs)
  {
    auto pieces = std::vector{slab};
    for (const auto& each : interiors)
    {
      auto rest = std::vector<vm::bbox3d>{};
      for (const auto& piece : pieces)
      {
        std::ranges::copy(subtract(piece, each), std::back_inserter(rest));
      }
      pieces = std::move(rest);
    }
    std::ranges::copy_if(pieces, std::back_inserter(result), hasVolume);
  }
  return result;
}

/** The boxes of a room, as the reference lowering builds them. */
std::vector<vm::bbox3d> lowerRoom(
  const vm::bbox3d& interior, const double wall, const std::vector<vm::bbox3d>& interiors)
{
  return lowerSlabs(shellSlabs(interior, wall), interiors);
}

/** A coordinate rounded to 3 decimals, as fingerprints compare coordinates. */
double roundedCoordinate(const double coordinate)
{
  return std::round(coordinate * 1000.0) / 1000.0;
}

/**
 * The space that some boxes fill together, as a grid along the axes whose cells are
 * filled or empty.
 */
class FilledGrid
{
private:
  /** The planes of the grid along each axis, ascending. */
  std::array<std::vector<double>, 3> m_planes;
  /** Whether each cell is filled, x fastest. */
  std::vector<bool> m_filled;

  size_t cells(const size_t axis) const { return m_planes[axis].size() - 1; }

  size_t indexOf(const std::array<size_t, 3>& cell) const
  {
    return cell[0] + cells(0) * (cell[1] + cells(1) * cell[2]);
  }

  /** Calls f with each cell, as {x, y, z} indices. */
  void forEachCell(const std::function<void(const std::array<size_t, 3>&)>& f) const
  {
    for (size_t z = 0; z < cells(2); ++z)
    {
      for (size_t y = 0; y < cells(1); ++y)
      {
        for (size_t x = 0; x < cells(0); ++x)
        {
          f({x, y, z});
        }
      }
    }
  }

  /**
   * Whether the plane with the given index along the axis can go: whether the cells on
   * either side of it are filled alike everywhere.
   */
  bool isRedundant(const size_t axis, const size_t plane) const
  {
    auto result = true;
    forEachCell([&](const auto& cell) {
      if (cell[axis] == plane)
      {
        auto before = cell;
        --before[axis];
        result = result && m_filled[indexOf(cell)] == m_filled[indexOf(before)];
      }
    });
    return result;
  }

  /** Removes a plane, merging the cells on either side of it. */
  void removePlane(const size_t axis, const size_t plane)
  {
    auto planes = m_planes;
    planes[axis].erase(planes[axis].begin() + static_cast<std::ptrdiff_t>(plane));
    auto merged = FilledGrid{std::move(planes)};
    forEachCell([&](const auto& cell) {
      if (cell[axis] != plane)
      {
        auto target = cell;
        target[axis] -= cell[axis] > plane ? 1 : 0;
        merged.m_filled[merged.indexOf(target)] = m_filled[indexOf(cell)];
      }
    });
    *this = std::move(merged);
  }

  explicit FilledGrid(std::array<std::vector<double>, 3> planes)
    : m_planes{std::move(planes)}
    , m_filled(cells(0) * cells(1) * cells(2), false)
  {
  }

public:
  /**
   * The grid of the boxes, rounded to 3 decimals, with only the planes where the filled
   * space changes, so that it is the same however the space is cut into boxes.
   */
  explicit FilledGrid(const std::vector<vm::bbox3d>& boxes)
  {
    auto rounded = std::vector<vm::bbox3d>{};
    for (const auto& box : boxes)
    {
      auto each = vm::bbox3d{};
      for (size_t k = 0; k < 3; ++k)
      {
        each.min[k] = roundedCoordinate(box.min[k]);
        each.max[k] = roundedCoordinate(box.max[k]);
        m_planes[k].push_back(each.min[k]);
        m_planes[k].push_back(each.max[k]);
      }
      rounded.push_back(each);
    }
    for (auto& planes : m_planes)
    {
      std::ranges::sort(planes);
      const auto [first, last] = std::ranges::unique(planes);
      planes.erase(first, last);
      if (planes.empty())
      {
        planes.push_back(0.0);
      }
    }

    m_filled.assign(cells(0) * cells(1) * cells(2), false);
    forEachCell([&](const auto& cell) {
      auto center = vm::vec3d{};
      for (size_t k = 0; k < 3; ++k)
      {
        center[k] = (m_planes[k][cell[k]] + m_planes[k][cell[k] + 1]) / 2.0;
      }
      m_filled[indexOf(cell)] = std::ranges::any_of(
        rounded, [&](const auto& box) { return box.contains(center); });
    });

    for (size_t k = 0; k < 3; ++k)
    {
      for (auto plane = cells(k) > 0 ? cells(k) - 1 : 0; plane > 0; --plane)
      {
        if (isRedundant(k, plane))
        {
          removePlane(k, plane);
        }
      }
    }
  }

  /** The planes relative to the given origin and the filled cells, as text. */
  std::string toString(const vm::vec3d& origin) const
  {
    auto result = std::string{};
    for (size_t k = 0; k < 3; ++k)
    {
      for (const auto plane : m_planes[k])
      {
        result += formatNumber(roundedCoordinate(plane - origin[k])) + ",";
      }
      result += ";";
    }
    for (const auto filled : m_filled)
    {
      result += filled ? '1' : '0';
    }
    return result;
  }
};

/**
 * A fingerprint of the geometry of some brushes that ignores where they are and how they
 * are ordered: a hash, relative to the minimum of all of them, of the space that the
 * axis-aligned boxes fill together, however it is cut into boxes, and of the vertices of
 * the other brushes, rounded to 3 decimals, sorted per brush, with the brushes sorted.
 */
std::string geometryFingerprint(
  const std::vector<vm::bbox3d>& boxes,
  const std::vector<std::vector<vm::vec3d>>& otherBrushes)
{
  auto origin = std::optional<vm::vec3d>{};
  for (const auto& box : boxes)
  {
    origin = origin ? vm::min(*origin, box.min) : box.min;
  }
  for (const auto& vertices : otherBrushes)
  {
    for (const auto& vertex : vertices)
    {
      origin = origin ? vm::min(*origin, vertex) : vertex;
    }
  }
  if (!origin)
  {
    origin = vm::vec3d{};
  }

  auto brushStrings = std::vector<std::string>{};
  for (const auto& vertices : otherBrushes)
  {
    auto vertexStrings = std::vector<std::string>{};
    for (const auto& vertex : vertices)
    {
      vertexStrings.push_back(formatVec3(vertex - *origin));
    }
    std::ranges::sort(vertexStrings);
    brushStrings.push_back(kdl::str_join(vertexStrings, ","));
  }
  std::ranges::sort(brushStrings);

  const auto text = boxes.empty() ? std::string{} : FilledGrid{boxes}.toString(*origin);
  return sha256Hex(text + "|" + kdl::str_join(brushStrings, ";"))
    .substr(0, FingerprintLength);
}

std::string boxesFingerprint(const std::vector<vm::bbox3d>& boxes)
{
  return geometryFingerprint(boxes, {});
}

/** The fingerprint of the carrier's brushes. */
std::string carrierFingerprint(const mdl::EntityNode& carrier)
{
  auto boxes = std::vector<vm::bbox3d>{};
  auto otherBrushes = std::vector<std::vector<vm::vec3d>>{};
  for (const auto* child : carrier.children())
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
    {
      if (isAxialBox(brushNode->brush()))
      {
        boxes.push_back(brushNode->logicalBounds());
      }
      else
      {
        otherBrushes.push_back(brushNode->brush().vertexPositions());
      }
    }
  }
  return geometryFingerprint(boxes, otherBrushes);
}


/** A room as its walls are built: around an interior, with walls of a thickness. */
struct RoomShape
{
  std::string name;
  vm::bbox3d interior;
  double wall = 0.0;
};

/** Where each room that has parameters and brushes stands, by carrier. */
using Shapes = std::map<const mdl::EntityNode*, RoomShape>;

/**
 * Where a room stands now: an interior of the size that its parameters give, at the
 * minimum of its carrier's brushes plus its walls. Nothing if the room has no
 * parameters or no brushes.
 */
std::optional<RoomShape> shapeOf(const BuiltRoom& room)
{
  if (!room.params || !room.carrier->hasChildren())
  {
    return std::nullopt;
  }
  const auto min = room.carrier->logicalBounds().min + vm::vec3d::fill(room.params->wall);
  return RoomShape{room.name, {min, min + room.params->size}, room.params->wall};
}

Shapes shapesOf(const std::vector<BuiltRoom>& rooms)
{
  auto result = Shapes{};
  for (const auto& room : rooms)
  {
    if (auto shape = shapeOf(room))
    {
      result.emplace(room.carrier, std::move(*shape));
    }
  }
  return result;
}

/** The interiors of the rooms that have a shape, in the order of the rooms. */
std::vector<vm::bbox3d> interiorsOf(
  const std::vector<BuiltRoom>& rooms, const Shapes& shapes)
{
  auto result = std::vector<vm::bbox3d>{};
  for (const auto& room : rooms)
  {
    if (const auto it = shapes.find(room.carrier); it != shapes.end())
    {
      result.push_back(it->second.interior);
    }
  }
  return result;
}

/** The room with the given name, if exactly one room has it. */
const BuiltRoom* uniqueRoom(
  const std::vector<BuiltRoom>& rooms, const std::string_view name)
{
  const BuiltRoom* result = nullptr;
  for (const auto& room : rooms)
  {
    if (room.name == name)
    {
      if (result)
      {
        return nullptr;
      }
      result = &room;
    }
  }
  return result;
}

std::string roomGroupName(const std::string_view name)
{
  return fmt::format("{}{}", RoomGroupPrefix, name);
}

/**
 * The built room of a carrier. Its status is left hand_edited, since it depends on the
 * other rooms and the connections; collectBuiltRooms determines it.
 */
BuiltRoom builtRoomOfCarrier(const mdl::EntityNode& carrier)
{
  const auto& entity = carrier.entity();
  auto name = *entity.property(Keys::Name);
  auto params = parseRoomParams(entity);

  const auto* group = dynamic_cast<const mdl::GroupNode*>(carrier.parent());
  if (group && group->name() != roomGroupName(name))
  {
    group = nullptr;
  }
  return BuiltRoom{
    &carrier, std::move(name), std::move(params), RoomStatus::HandEdited, group};
}

Json placementToJson(const Placement& place)
{
  auto result = Json::object();
  result.set(std::string{toString(place.relation)}, place.room);
  result.set("gap", place.gap);
  return result;
}

/**
 * The paths of the carriers that an ambiguous name could mean, as an
 * `ambiguous_room_name` error lists them.
 *
 * @param what what shares the name, for the message, as in `2 built rooms are named
 * "A"`
 */
CliError ambiguousName(
  const std::string_view opName,
  const std::string_view what,
  const std::vector<const mdl::EntityNode*>& carriers)
{
  auto paths = std::vector<std::string>{};
  for (const auto* carrier : carriers)
  {
    paths.push_back(nodePath(*carrier));
  }
  return CliError{
    ErrorCode::AmbiguousRoomName,
    fmt::format(
      "{}: {} ({}); rename, detach or delete all but one",
      opName,
      what,
      kdl::str_join(paths, ", "))}
    .withCandidates(std::move(paths));
}

/**
 * The built room with the given name that an op acts on, whether or not its parameters
 * are well-formed.
 *
 * @param action what the op does with the room, for messages, as in `place against`
 * @throw CliError `node_not_found` if no room has the name, and `ambiguous_room_name`
 * if several rooms have it
 */
const BuiltRoom& findNamedRoom(
  const std::string_view opName,
  const std::string_view action,
  const std::vector<BuiltRoom>& rooms,
  const std::string& name)
{
  auto candidates = std::vector<const BuiltRoom*>{};
  for (const auto& room : rooms)
  {
    if (room.name == name)
    {
      candidates.push_back(&room);
    }
  }

  if (candidates.empty())
  {
    throw CliError{
      ErrorCode::NodeNotFound,
      fmt::format(
        "{}: there is no built room named \"{}\" to {}; describe lists the built rooms",
        opName,
        name,
        action)};
  }
  if (candidates.size() > 1)
  {
    throw ambiguousName(
      opName,
      fmt::format("{} built rooms are named \"{}\"", candidates.size(), name),
      candidates | std::views::transform([](const auto* room) { return room->carrier; })
        | kdl::ranges::to<std::vector>());
  }
  return *candidates.front();
}

/**
 * The built room with the given name that an op builds with.
 *
 * @param action what the op does with the room, for messages, as in `place against`
 * @throw CliError as findNamedRoom does, and `op_failed` if the room has no brushes or
 * malformed parameter keys
 */
const BuiltRoom& findRoom(
  const std::string_view opName,
  const std::string_view action,
  const std::vector<BuiltRoom>& rooms,
  const std::string& name)
{
  const auto& room = findNamedRoom(opName, action, rooms, name);
  const auto path = nodePath(*room.carrier);
  if (!room.params)
  {
    throw opFailed(
      opName,
      fmt::format(
        "cannot {} room \"{}\", whose parameter keys are missing or malformed",
        action,
        name),
      path);
  }
  if (!room.carrier->hasChildren())
  {
    throw opFailed(
      opName,
      fmt::format("cannot {} room \"{}\", which has no brushes", action, name),
      path);
  }
  return room;
}

/** The axes of a placement. */
struct PlacementAxes
{
  /** The horizontal axis along which the room stands next to the other. */
  size_t axis = 0;
  /** Whether it stands towards greater coordinates. */
  bool positive = true;
  /** The other horizontal axis, along which it is centred. */
  size_t other = 1;
};

PlacementAxes placementAxes(const Relation relation)
{
  const auto axis =
    relation == Relation::EastOf || relation == Relation::WestOf ? size_t{0} : size_t{1};
  return {axis, relation == Relation::EastOf || relation == Relation::NorthOf, 1 - axis};
}

/**
 * The minimum along the axis of a room of the given size centred on the reference
 * along it, rounded half up as in the reference lowering.
 */
double centredMin(const vm::bbox3d& reference, const double size, const size_t axis)
{
  return reference.min[axis] + std::floor((reference.size()[axis] - size) / 2 + 0.5);
}

/**
 * The interior of a room of the given size placed next to the reference room: on the
 * given side at the gap, centred on the other horizontal axis, at the same floor
 * height.
 */
vm::bbox3d placedInterior(
  const vm::bbox3d& reference, const vm::vec3d& size, const Placement& place)
{
  const auto [axis, positive, other] = placementAxes(place.relation);
  auto min = vm::vec3d{0, 0, reference.min.z()};
  min[axis] = positive ? reference.max[axis] + place.gap
                       : reference.min[axis] - place.gap - size[axis];
  min[other] = centredMin(reference, size[other], other);
  return {min, min + size};
}

/**
 * Whether the interior stands where the placement puts it next to the reference: at the
 * gap on the given side, on the same floor, and beside it along the other horizontal
 * axis.
 */
bool standsAt(
  const vm::bbox3d& interior, const vm::bbox3d& reference, const Placement& place)
{
  const auto [axis, positive, other] = placementAxes(place.relation);
  const auto gap = positive ? interior.min[axis] - reference.max[axis]
                            : reference.min[axis] - interior.max[axis];
  const auto beside = std::min(interior.max[other], reference.max[other])
                      - std::max(interior.min[other], reference.min[other]);
  return std::abs(gap - place.gap) <= OverlapTolerance
         && std::abs(interior.min.z() - reference.min.z()) <= OverlapTolerance
         && beside > 0;
}

/**
 * Where a room placed against a reference stands once the reference and the room were
 * rebuilt: placed against the rebuilt reference with its new size, keeping how far it
 * was moved along the reference from the centre. Nothing if the room does not stand
 * where its placement puts it next to the reference as it was, which makes the
 * placement stale.
 */
std::optional<vm::bbox3d> replacedInterior(
  const vm::bbox3d& interior,
  const vm::vec3d& size,
  const Placement& place,
  const vm::bbox3d& referenceBefore,
  const vm::bbox3d& referenceAfter)
{
  if (!standsAt(interior, referenceBefore, place))
  {
    return std::nullopt;
  }
  const auto other = placementAxes(place.relation).other;
  const auto offset =
    interior.min[other] - centredMin(referenceBefore, interior.size()[other], other);
  auto result = placedInterior(referenceAfter, size, place);
  result.min[other] += offset;
  result.max[other] += offset;
  return result;
}

/** How messages name a carrier: `room "A"` or `connection A-B`. */
std::string describeCarrier(const mdl::EntityNode& carrier)
{
  const auto& entity = carrier.entity();
  if (isRoomCarrier(carrier))
  {
    return fmt::format("room \"{}\"", *entity.property(Keys::Name));
  }
  const auto* from = entity.property(Keys::From);
  const auto* to = entity.property(Keys::To);
  return fmt::format("connection {}-{}", from ? *from : "?", to ? *to : "?");
}

/** The space that a carrier's brushes take, which new walls must not overlap. */
struct Obstacle
{
  vm::bbox3d box;
  const mdl::EntityNode* carrier = nullptr;
};

/**
 * The bounds of the carriers of the built rooms and connections that have brushes,
 * except the given ones.
 */
std::vector<Obstacle> carrierObstacles(
  const mdl::WorldNode& worldNode, const std::vector<const mdl::EntityNode*>& except = {})
{
  auto result = std::vector<Obstacle>{};
  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (
      (isRoomCarrier(*entityNode) || isConnectionCarrier(*entityNode))
      && entityNode->hasChildren() && !kdl::ranges::contains(except, entityNode))
    {
      result.push_back({entityNode->logicalBounds(), entityNode});
    }
  }
  return result;
}

/**
 * Fails if the box overlaps one of the obstacles by more than OverlapTolerance. Boxes
 * that only touch do not overlap.
 *
 * @param what what would overlap, for the message, as in `the walls of room "B"`
 * @param hint how to avoid the overlap, for the message
 */
void checkNoOverlap(
  const std::string_view opName,
  const std::string_view what,
  const vm::bbox3d& box,
  const std::vector<Obstacle>& obstacles,
  const std::string_view hint)
{
  for (const auto& obstacle : obstacles)
  {
    if (overlaps(box, obstacle.box, OverlapTolerance))
    {
      const auto path = nodePath(*obstacle.carrier);
      throw CliError{
        ErrorCode::LayoutOverlap,
        fmt::format(
          "{}: {} would overlap {} ({}); {}",
          opName,
          what,
          describeCarrier(*obstacle.carrier),
          path,
          hint),
        path};
    }
  }
}

/**
 * Fails with `locked` if the node is locked, without --allow-locked.
 *
 * @param what what the node is, for the message, as in `room "A"`
 * @param action what the op would do to it, for the message, as in `cut its walls`
 */
void checkUnlocked(
  const OpContext& context,
  const mdl::Node& node,
  const std::string_view what,
  const std::string_view action)
{
  if (!context.allowLocked && node.locked())
  {
    const auto path = nodePath(node);
    throw CliError{
      ErrorCode::Locked,
      fmt::format(
        "{}: {} ({}) is locked; pass --allow-locked to {} anyway",
        context.opName,
        what,
        path,
        action),
      path};
  }
}

/**
 * The default layer, which built rooms and connections are created in.
 *
 * @throw CliError `locked` if it is locked, without --allow-locked
 */
mdl::LayerNode& defaultLayerToBuildIn(const OpContext& context)
{
  auto& layer = *context.map.worldNode().defaultLayer();
  checkUnlocked(
    context,
    layer,
    "the default layer, where built rooms and connections are created,",
    "edit it");
  return layer;
}

/**
 * A new carrier with the given keys that holds a brush for each box, in a new group with
 * the given name. `_tbcli_fp` is added to the keys: the fingerprint of the boxes.
 */
std::unique_ptr<mdl::GroupNode> newCarrierGroup(
  const OpContext& context,
  const std::string& groupName,
  std::vector<mdl::EntityProperty> properties,
  const std::vector<vm::bbox3d>& boxes)
{
  properties.emplace_back(Keys::Fingerprint, boxesFingerprint(boxes));

  const auto builder = brushBuilderOf(context.map);
  auto carrier = std::make_unique<mdl::EntityNode>(mdl::Entity{std::move(properties)});
  for (const auto& box : boxes)
  {
    auto brush = builder.createCuboid(box, mdl::BrushFace::NoMaterialName);
    if (!brush)
    {
      throw opFailed(
        context.opName,
        fmt::format(
          "cannot create the brushes of {}: {}",
          groupName,
          std::get<Error>(brush.error()).msg));
    }
    carrier->addChild(new mdl::BrushNode{std::move(brush).value()});
  }

  auto group = std::make_unique<mdl::GroupNode>(mdl::Group{groupName});
  group->addChild(carrier.release());
  return group;
}

/** Adds a new carrier's group to the layer and reports it as created. */
void addCarrierGroup(
  OpContext& context, mdl::LayerNode& layer, std::unique_ptr<mdl::GroupNode> group)
{
  if (mdl::addNodes(context.map, {{&layer, {group.get()}}}).empty())
  {
    throw opFailed(
      context.opName, fmt::format("cannot add {} to the map", group->name()));
  }
  context.outcome.created.push_back(group.release());
}

std::string connectionGroupName(const std::string_view from, const std::string_view to)
{
  return fmt::format("{}{}-{}", ConnectionGroupPrefix, from, to);
}

/** The parameters of a connection. */
struct ConnectionParams
{
  /** The width of the corridor's interior. */
  double width = 0.0;
  /** The height of the corridor's interior. */
  double height = 0.0;
  /**
   * How far the corridor is moved from the middle of the wall that the rooms share,
   * towards greater coordinates.
   */
  double offset = 0.0;
};

/** The parameters that a connection's carrier stores, or nothing if a key is malformed.
 */
std::optional<ConnectionParams> parseConnectionParams(const mdl::Entity& entity)
{
  const auto number = [&](const std::string& key) {
    const auto* value = entity.property(key);
    return value ? parseDecimal(*value) : std::nullopt;
  };
  const auto width = number(Keys::Width);
  const auto height = number(Keys::Height);
  const auto offset = number(Keys::Offset);
  return width && height && offset && *width > 0 && *height > 0
           ? std::optional{ConnectionParams{*width, *height, *offset}}
           : std::nullopt;
}

/** A connection as the map holds it. */
struct BuiltConnection
{
  const mdl::EntityNode* carrier = nullptr;
  /** The name of the room it starts at, empty if the key is missing. */
  std::string from;
  /** The name of the room it leads to, empty if the key is missing. */
  std::string to;
  /** Unset if the carrier's parameter keys are missing or malformed. */
  std::optional<ConnectionParams> params;

  bool joins(const std::string_view room) const { return from == room || to == room; }

  /** `A-B`, as its group names it after the rooms that it joins. */
  std::string name() const { return fmt::format("{}-{}", from, to); }
};

/** The connections of the map, in file order. */
std::vector<BuiltConnection> collectConnections(const mdl::WorldNode& worldNode)
{
  auto result = std::vector<BuiltConnection>{};
  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (isConnectionCarrier(*entityNode))
    {
      const auto& entity = entityNode->entity();
      const auto* from = entity.property(Keys::From);
      const auto* to = entity.property(Keys::To);
      result.push_back(
        BuiltConnection{
          entityNode,
          from ? *from : std::string{},
          to ? *to : std::string{},
          parseConnectionParams(entity)});
    }
  }
  return result;
}

/**
 * The connection with the given name `A-B` that an op acts on; `B-A` names it too.
 *
 * @param action what the op does with the connection, for messages, as in `edit`
 * @throw CliError `node_not_found` if no connection joins the rooms, and
 * `ambiguous_room_name` if several do
 */
const BuiltConnection& findConnection(
  const std::string_view opName,
  const std::string_view action,
  const std::vector<BuiltConnection>& connections,
  const std::string& name)
{
  // checkArgs makes sure that the name is two room names joined by a dash
  const auto rooms = kdl::str_split(name, "-");
  contract_assert(rooms.size() == 2);

  auto candidates = std::vector<const BuiltConnection*>{};
  for (const auto& connection : connections)
  {
    if (
      (connection.from == rooms[0] && connection.to == rooms[1])
      || (connection.from == rooms[1] && connection.to == rooms[0]))
    {
      candidates.push_back(&connection);
    }
  }

  if (candidates.empty())
  {
    throw CliError{
      ErrorCode::NodeNotFound,
      fmt::format(
        "{}: there is no connection between rooms \"{}\" and \"{}\" to {}",
        opName,
        rooms[0],
        rooms[1],
        action)};
  }
  if (candidates.size() > 1)
  {
    throw ambiguousName(
      opName,
      fmt::format(
        "{} connections join rooms \"{}\" and \"{}\"",
        candidates.size(),
        rooms[0],
        rooms[1]),
      candidates | std::views::transform([](const auto* connection) {
        return connection->carrier;
      }) | kdl::ranges::to<std::vector>());
  }
  return *candidates.front();
}

/**
 * The parameters of a connection that a rebuild builds with.
 *
 * @throw CliError `op_failed` if they are missing or malformed
 */
const ConnectionParams& paramsOf(
  const std::string_view opName, const BuiltConnection& connection)
{
  if (!connection.params)
  {
    throw opFailed(
      opName,
      fmt::format(
        "cannot rebuild connection {}, whose parameter keys are missing or malformed",
        connection.name()),
      nodePath(*connection.carrier));
  }
  return *connection.params;
}

/** The horizontal axes of a corridor: the one it runs along, and the one across it. */
struct CorridorAxes
{
  size_t along = 0;
  size_t across = 1;

  static CorridorAxes runningAlong(const size_t axis) { return {axis, 1 - axis}; }
};

/** A straight corridor between two rooms, as the reference lowering builds it. */
struct Corridor
{
  /** Its space, from one room's interior to the other's, through both walls. */
  vm::bbox3d interior;
  /** The part of the interior between the rooms' outer wall faces. */
  vm::bbox3d segment;
  CorridorAxes axes;
  /** The thickness of its walls, floor and ceiling. */
  double wall = 0.0;
};

std::string_view axisName(const size_t axis)
{
  return axis == 0 ? "x" : "y";
}

/**
 * The axes of a corridor between the interiors: it runs along the horizontal axis along
 * which they lie apart, with walls that face each other over a positive width. Nothing
 * if there is no such axis.
 */
std::optional<CorridorAxes> corridorAxesBetween(
  const vm::bbox3d& fromInterior, const vm::bbox3d& toInterior)
{
  for (const auto axis : {size_t{0}, size_t{1}})
  {
    const auto axes = CorridorAxes::runningAlong(axis);
    const auto apart = toInterior.min[axes.along] >= fromInterior.max[axes.along]
                       || fromInterior.min[axes.along] >= toInterior.max[axes.along];
    const auto shared =
      std::min(fromInterior.max[axes.across], toInterior.max[axes.across])
      - std::max(fromInterior.min[axes.across], toInterior.min[axes.across]);
    if (apart && shared > 0)
    {
      return axes;
    }
  }
  return std::nullopt;
}

/**
 * The corridor between two rooms that stand side by side, as in the reference lowering:
 * on their floor, centred on the wall that they share and moved by the offset, rounded
 * half up relative to the start of the shared wall, so that moving both rooms does not
 * change where it is (for integer coordinates the same as rounding the centre, as the
 * reference lowering does). Its walls are as thick as the thicker of the rooms' walls.
 * The error, if the rooms do not allow a corridor, is returned rather than thrown, so
 * that the status of a room can be determined without one:
 * `layout_corridor_does_not_fit` if the rooms do not stand side by side,
 * their walls touch so that the corridor would have no length, or the corridor reaches
 * beyond the wall that they share, `layout_floor_mismatch` if their floors are at
 * different heights, and `layout_corridor_too_tall` if the corridor would reach above a
 * room's ceiling
 */
std::variant<Corridor, CliError> tryCorridorBetween(
  const std::string_view opName,
  const RoomShape& from,
  const RoomShape& to,
  const ConnectionParams& params)
{
  const auto& fromInterior = from.interior;
  const auto& toInterior = to.interior;

  const auto axes = corridorAxesBetween(fromInterior, toInterior);
  if (!axes)
  {
    return CliError{
      ErrorCode::LayoutCorridorDoesNotFit,
      fmt::format(
        "{}: rooms \"{}\" and \"{}\" do not stand side by side, apart along x or y with "
        "walls that face each other, so no straight corridor joins them",
        opName,
        from.name,
        to.name)};
  }
  const auto [along, across] = *axes;

  const auto floor = fromInterior.min.z();
  if (std::abs(floor - toInterior.min.z()) > OverlapTolerance)
  {
    return CliError{
      ErrorCode::LayoutFloorMismatch,
      fmt::format(
        "{}: the floor of room \"{}\" is at z {}, that of room \"{}\" at z {}; "
        "corridors join rooms on the same floor",
        opName,
        from.name,
        formatNumber(floor),
        to.name,
        formatNumber(toInterior.min.z()))};
  }

  const auto sharedMin = std::max(fromInterior.min[across], toInterior.min[across]);
  const auto sharedMax = std::min(fromInterior.max[across], toInterior.max[across]);
  // rounded half up, as in the reference lowering
  const auto acrossMin =
    sharedMin
    + std::floor((sharedMax - sharedMin) / 2 + params.offset - params.width / 2 + 0.5);
  const auto acrossMax = acrossMin + params.width;
  if (
    acrossMin < sharedMin - OverlapTolerance || acrossMax > sharedMax + OverlapTolerance)
  {
    return CliError{
      ErrorCode::LayoutCorridorDoesNotFit,
      fmt::format(
        "{}: the corridor would span {} {} to {}, beyond the wall that rooms \"{}\" and "
        "\"{}\" share from {} to {}; make it narrower or change its offset",
        opName,
        axisName(across),
        formatNumber(acrossMin),
        formatNumber(acrossMax),
        from.name,
        to.name,
        formatNumber(sharedMin),
        formatNumber(sharedMax))};
  }

  const auto top = floor + params.height;
  const auto& lower = fromInterior.max.z() <= toInterior.max.z() ? from : to;
  const auto ceiling = std::min(fromInterior.max.z(), toInterior.max.z());
  if (top > ceiling + OverlapTolerance)
  {
    return CliError{
      ErrorCode::LayoutCorridorTooTall,
      fmt::format(
        "{}: the corridor would reach z {}, above the ceiling of room \"{}\" at z {}; "
        "make it at most {} tall",
        opName,
        formatNumber(top),
        lower.name,
        formatNumber(ceiling),
        formatNumber(ceiling - floor))};
  }

  // the rooms in the order the corridor runs along its axis
  const auto fromFirst = fromInterior.max[along] <= toInterior.min[along];
  const auto& [first, second] = fromFirst ? std::pair{&from, &to} : std::pair{&to, &from};

  auto interior = vm::bbox3d{};
  interior.min[along] = first->interior.max[along];
  interior.max[along] = second->interior.min[along];
  interior.min[across] = acrossMin;
  interior.max[across] = acrossMax;
  interior.min[2] = floor;
  interior.max[2] = top;

  auto segment = interior;
  segment.min[along] += first->wall;
  segment.max[along] -= second->wall;
  if (segment.max[along] - segment.min[along] <= OverlapTolerance)
  {
    return CliError{
      ErrorCode::LayoutCorridorDoesNotFit,
      fmt::format(
        "{}: the walls of rooms \"{}\" and \"{}\" touch, so a corridor between them "
        "would have no length; place them further apart",
        opName,
        from.name,
        to.name)};
  }

  return Corridor{interior, segment, *axes, std::max(from.wall, to.wall)};
}

/**
 * The corridor between two rooms, as tryCorridorBetween builds it.
 *
 * @throw CliError the error of tryCorridorBetween, if the rooms do not allow a corridor
 */
Corridor corridorBetween(
  const std::string_view opName,
  const RoomShape& from,
  const RoomShape& to,
  const ConnectionParams& params)
{
  auto result = tryCorridorBetween(opName, from, to, params);
  if (auto* error = std::get_if<CliError>(&result))
  {
    throw std::move(*error);
  }
  return std::get<Corridor>(std::move(result));
}

/**
 * The walls, floor and ceiling of a corridor along the segment between the rooms' outer
 * wall faces, as in the reference lowering: the floor and ceiling span the side walls.
 */
std::vector<vm::bbox3d> corridorSlabs(const Corridor& corridor)
{
  const auto& segment = corridor.segment;
  const auto across = corridor.axes.across;
  const auto thickness = corridor.wall;

  auto floor = segment;
  floor.min[across] -= thickness;
  floor.max[across] += thickness;
  floor.max[2] = segment.min[2];
  floor.min[2] = segment.min[2] - thickness;

  auto ceiling = floor;
  ceiling.min[2] = segment.max[2];
  ceiling.max[2] = segment.max[2] + thickness;

  auto low = segment;
  low.max[across] = segment.min[across];
  low.min[across] = segment.min[across] - thickness;

  auto high = segment;
  high.min[across] = segment.max[across];
  high.max[across] = segment.max[across] + thickness;

  return {floor, ceiling, low, high};
}

/** The box that a corridor's brushes fill: its segment with its walls around it. */
vm::bbox3d corridorShell(const Corridor& corridor)
{
  auto grow = vm::vec3d{0, 0, corridor.wall};
  grow[corridor.axes.across] = corridor.wall;
  return {corridor.segment.min - grow, corridor.segment.max + grow};
}

/**
 * The boxes of a room's walls, floor and ceiling, with the openings of its connections
 * cut out, as the reference lowering builds them.
 */
std::vector<vm::bbox3d> roomBoxes(
  const RoomShape& shape, const std::vector<vm::bbox3d>& openings)
{
  return lowerSlabs(shellSlabs(shape.interior, shape.wall), openings);
}

/** The boxes of a corridor's walls, floor and ceiling. */
std::vector<vm::bbox3d> corridorBoxes(const Corridor& corridor)
{
  return lowerSlabs(corridorSlabs(corridor), {corridor.interior});
}

/**
 * The built rooms and connections of a map and where the rooms stand, which statuses
 * and rebuilds are determined against.
 */
struct Layout
{
  std::vector<BuiltRoom> rooms;
  std::vector<BuiltConnection> connections;
  Shapes shapes;
};

/**
 * The corridor that a connection with the given parameters builds between its rooms
 * where they stand, or nothing if a room is missing, ambiguous or has no shape, or the
 * rooms no longer allow the corridor.
 */
std::optional<Corridor> corridorIfPossible(
  const BuiltConnection& connection, const ConnectionParams& params, const Layout& layout)
{
  const auto shapeNamed = [&](const std::string& name) -> const RoomShape* {
    const auto* room = uniqueRoom(layout.rooms, name);
    const auto it = room ? layout.shapes.find(room->carrier) : layout.shapes.end();
    return it != layout.shapes.end() ? &it->second : nullptr;
  };

  const auto* from = shapeNamed(connection.from);
  const auto* to = shapeNamed(connection.to);
  if (!from || !to)
  {
    return std::nullopt;
  }
  const auto result = tryCorridorBetween({}, *from, *to, params);
  const auto* corridor = std::get_if<Corridor>(&result);
  return corridor ? std::optional{*corridor} : std::nullopt;
}

/**
 * The openings that the connections which join the named room cut into its walls, with
 * each connection's corridor as `corridorOf` builds it, or nothing if it builds none for
 * one of them.
 */
template <typename CorridorOf>
std::optional<std::vector<vm::bbox3d>> openingsOf(
  const std::string_view room,
  const std::vector<BuiltConnection>& connections,
  const CorridorOf& corridorOf)
{
  auto result = std::vector<vm::bbox3d>{};
  for (const auto& connection : connections)
  {
    if (connection.joins(room))
    {
      const std::optional<Corridor> corridor = corridorOf(connection);
      if (!corridor)
      {
        return std::nullopt;
      }
      result.push_back(corridor->interior);
    }
  }
  return result;
}

/** Whether the carrier's `_tbcli_fp` is the given fingerprint. */
bool hasStoredFingerprint(const mdl::EntityNode& carrier, const std::string& fingerprint)
{
  const auto* stored = carrier.entity().property(Keys::Fingerprint);
  return stored && *stored == fingerprint;
}

/**
 * Whether a room's brushes match what its parameters build. Its carrier's bounds must
 * be its interior with its walls around it, which notices a size or wall key edited by
 * hand. Its brushes, normalised for translation and compared on geometry only, must
 * have the fingerprint that tbcli stored when it last built or cut them, or that of
 * what its parameters build where it stands: its walls, floor and ceiling with the
 * opening of every connection that joins it cut out. The stored fingerprint keeps a
 * room rebuildable when a room it is joined to is moved alone or a connection is
 * detached, which leave its own brushes as they were; the built one accepts rooms whose
 * fingerprint tbcli did not write, such as rooms made by hand.
 */
RoomStatus roomStatus(const BuiltRoom& room, const Layout& layout)
{
  const auto shape = layout.shapes.find(room.carrier);
  if (
    shape == layout.shapes.end()
    || !vm::is_equal(
      room.carrier->logicalBounds().size(),
      shape->second.interior.expand(shape->second.wall).size(),
      OverlapTolerance))
  {
    return RoomStatus::HandEdited;
  }

  const auto fingerprint = carrierFingerprint(*room.carrier);
  if (hasStoredFingerprint(*room.carrier, fingerprint))
  {
    return RoomStatus::Rebuildable;
  }
  const auto openings =
    openingsOf(room.name, layout.connections, [&](const BuiltConnection& connection) {
      return connection.params
               ? corridorIfPossible(connection, *connection.params, layout)
               : std::nullopt;
    });
  return openings && boxesFingerprint(roomBoxes(shape->second, *openings)) == fingerprint
           ? RoomStatus::Rebuildable
           : RoomStatus::HandEdited;
}

/**
 * Whether a connection's brushes match what its parameters build: normalised for
 * translation and compared on geometry only, they must have the fingerprint that tbcli
 * stored when it last built them, or that of the corridor that its parameters build
 * between its rooms where they stand.
 */
RoomStatus connectionStatus(const BuiltConnection& connection, const Layout& layout)
{
  if (!connection.params || !connection.carrier->hasChildren())
  {
    return RoomStatus::HandEdited;
  }

  const auto fingerprint = carrierFingerprint(*connection.carrier);
  if (hasStoredFingerprint(*connection.carrier, fingerprint))
  {
    return RoomStatus::Rebuildable;
  }
  const auto corridor = corridorIfPossible(connection, *connection.params, layout);
  return corridor && boxesFingerprint(corridorBoxes(*corridor)) == fingerprint
           ? RoomStatus::Rebuildable
           : RoomStatus::HandEdited;
}

/** The built rooms with their statuses and the connections of the map. */
Layout layoutOf(const mdl::WorldNode& worldNode)
{
  auto result = Layout{};
  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (isRoomCarrier(*entityNode))
    {
      result.rooms.push_back(builtRoomOfCarrier(*entityNode));
    }
  }
  result.connections = collectConnections(worldNode);
  result.shapes = shapesOf(result.rooms);
  for (auto& room : result.rooms)
  {
    room.status = roomStatus(room, result);
  }
  return result;
}

/**
 * A cuboid brush that fills the box, with the materials and UV of the faces of the
 * given brush that face the same way, if one is given.
 */
mdl::Brush cuboidLike(
  const OpContext& context,
  const mdl::BrushBuilder& builder,
  const vm::bbox3d& box,
  const mdl::Brush* original)
{
  auto brush = builder.createCuboid(box, mdl::BrushFace::NoMaterialName);
  if (!brush)
  {
    throw opFailed(
      context.opName,
      fmt::format("cannot build a wall: {}", std::get<Error>(brush.error()).msg));
  }
  auto result = std::move(brush).value();
  if (original)
  {
    for (auto& face : result.faces())
    {
      if (const auto index = original->findFace(face.normal()))
      {
        const auto& originalFace = original->face(*index);
        if (const auto snapshot = originalFace.takeUvCoordSystemSnapshot())
        {
          face.restoreUvCoordSystemSnapshot(*snapshot);
        }
        face.copyAttributes(originalFace);
      }
    }
  }
  return result;
}

/** Updates the carrier's keys, as one change that reaches its linked copies. */
void updateCarrierKeys(
  OpContext& context,
  mdl::EntityNode& carrier,
  mdl::Entity entity,
  const std::string_view what)
{
  if (!mdl::updateNodeContents(
        context.map,
        std::string{context.opName},
        {{&carrier, mdl::NodeContents{std::move(entity)}}},
        mdl::collectContainingGroups({&carrier})))
  {
    throw opFailed(
      context.opName,
      fmt::format("cannot update the keys of {}", what),
      nodePath(carrier));
  }
}

/**
 * Cuts the opening out of a room's walls: each brush that reaches into it is replaced by
 * the boxes of it that lie outside, which keep its materials. The room's fingerprint is
 * set to its cut brushes. The carrier is reported as modified and the brushes that were
 * cut as removed.
 */
void cutOpening(OpContext& context, const BuiltRoom& room, const vm::bbox3d& opening)
{
  auto& map = context.map;
  // the rooms were collected from the map that this op edits
  auto& carrier = const_cast<mdl::EntityNode&>(*room.carrier);
  const auto builder = brushBuilderOf(map);

  auto cut = std::vector<mdl::Node*>{};
  auto pieces = std::vector<std::unique_ptr<mdl::BrushNode>>{};
  for (auto* child : carrier.children())
  {
    if (
      auto* brushNode = dynamic_cast<mdl::BrushNode*>(child);
      brushNode && overlaps(brushNode->logicalBounds(), opening))
    {
      for (const auto& box : subtract(brushNode->logicalBounds(), opening))
      {
        if (hasVolume(box))
        {
          pieces.push_back(
            std::make_unique<mdl::BrushNode>(
              cuboidLike(context, builder, box, &brushNode->brush())));
        }
      }
      cut.push_back(brushNode);
    }
  }

  if (!pieces.empty())
  {
    auto nodes =
      pieces
      | std::views::transform([](const auto& piece) -> mdl::Node* { return piece.get(); })
      | kdl::ranges::to<std::vector>();
    if (mdl::addNodes(map, {{&carrier, std::move(nodes)}}).empty())
    {
      throw opFailed(
        context.opName, fmt::format("cannot cut the walls of room \"{}\"", room.name));
    }
    for (auto& piece : pieces)
    {
      piece.release();
    }
  }
  // the pieces are added first, so that removing the cut brushes never empties the
  // carrier
  mdl::removeNodes(map, cut);

  auto entity = carrier.entity();
  entity.addOrUpdateProperty(Keys::Fingerprint, carrierFingerprint(carrier));
  updateCarrierKeys(context, carrier, std::move(entity), describeCarrier(carrier));

  context.outcome.modified.push_back(&carrier);
  std::ranges::copy(cut, std::back_inserter(context.outcome.removed));
}

/** Whether the node is inside a group that is linked to another. */
bool isInLinkedGroup(OpContext& context, const mdl::Node& node)
{
  auto& worldNode = context.map.worldNode();
  for (const auto* ancestor = node.parent(); ancestor; ancestor = ancestor->parent())
  {
    if (
      const auto* group = dynamic_cast<const mdl::GroupNode*>(ancestor);
      group && mdl::collectGroupsWithLinkId({&worldNode}, group->linkId()).size() > 1)
    {
      return true;
    }
  }
  return false;
}

/**
 * Fails with `connect_cuts_linked_room` if the room is inside a linked group, whose
 * copies would get the opening without the corridor.
 */
void checkNotLinked(OpContext& context, const BuiltRoom& room)
{
  if (isInLinkedGroup(context, *room.carrier))
  {
    const auto path = nodePath(*room.carrier);
    throw CliError{
      ErrorCode::ConnectCutsLinkedRoom,
      fmt::format(
        "{}: room \"{}\" ({}) is inside a linked group, and cutting its walls would cut "
        "every linked copy without a corridor",
        context.opName,
        room.name,
        path),
      path};
  }
}

/** A `hand_edited` error for a carrier whose brushes no longer match its parameters. */
CliError handEdited(
  const OpContext& context, const mdl::EntityNode& carrier, const std::string_view hint)
{
  const auto path = nodePath(carrier);
  return CliError{
    ErrorCode::HandEdited,
    fmt::format(
      "{}: {} ({}) is hand_edited: its brushes no longer match what its parameters "
      "build; {}",
      context.opName,
      describeCarrier(carrier),
      path,
      hint),
    path};
}

/**
 * A room whose walls a connection cuts.
 *
 * @throw CliError as findRoom does, `locked` if the room is locked, without
 * --allow-locked, `connect_cuts_linked_room` if it is inside a linked group, and
 * `hand_edited` if its brushes no longer match what its parameters build
 */
const BuiltRoom& roomToConnect(
  OpContext& context, const std::vector<BuiltRoom>& rooms, const std::string& name)
{
  const auto& room = findRoom(context.opName, "connect", rooms, name);
  checkUnlocked(context, *room.carrier, describeCarrier(*room.carrier), "cut its walls");
  checkNotLinked(context, room);
  if (room.status != RoomStatus::Rebuildable)
  {
    throw handEdited(context, *room.carrier, "its walls cannot be cut safely");
  }
  return room;
}

/**
 * Fails if a connection joins the two rooms already, in either direction, since
 * connections are named by the rooms that they join.
 */
void checkNotConnected(
  const OpContext& context, const std::string& from, const std::string& to)
{
  for (const auto& connection : collectConnections(context.map.worldNode()))
  {
    if (
      (connection.from == from && connection.to == to)
      || (connection.from == to && connection.to == from))
    {
      const auto path = nodePath(*connection.carrier);
      throw opFailed(
        context.opName,
        fmt::format(
          "rooms \"{}\" and \"{}\" are joined already by {} ({})",
          from,
          to,
          describeCarrier(*connection.carrier),
          path),
        path);
    }
  }
}

/**
 * The side of a box that a part of it lies on: the axis and direction in which the
 * part's centre is furthest from the box's centre, relative to the box's size. Ties go
 * to z, then x.
 */
std::pair<size_t, bool> sideOf(const vm::bbox3d& part, const vm::bbox3d& box)
{
  auto result = std::pair{size_t{2}, false};
  auto furthest = -1.0;
  for (const auto axis : {size_t{2}, size_t{0}, size_t{1}})
  {
    const auto half = box.size()[axis] / 2;
    const auto offset =
      half > 0 ? (part.center()[axis] - box.center()[axis]) / half : 0.0;
    if (std::abs(offset) > furthest)
    {
      furthest = std::abs(offset);
      result = {axis, offset > 0};
    }
  }
  return result;
}

/** The volume that two boxes share. */
double sharedVolume(const vm::bbox3d& a, const vm::bbox3d& b)
{
  auto result = 1.0;
  for (size_t k = 0; k < 3; ++k)
  {
    result *= std::max(0.0, std::min(a.max[k], b.max[k]) - std::max(a.min[k], b.min[k]));
  }
  return result;
}

/**
 * The old brush whose materials a rebuilt box takes: of the old brushes on the same side
 * of the carrier, the one that shares the most volume with the box once moved as the
 * carrier's minimum moved, else the first of them. Nothing if no old brush is on that
 * side.
 */
const mdl::BrushNode* materialSource(
  const vm::bbox3d& box,
  const vm::bbox3d& newBounds,
  const std::vector<const mdl::BrushNode*>& oldBrushes,
  const vm::bbox3d& oldBounds)
{
  const auto side = sideOf(box, newBounds);
  const auto delta = newBounds.min - oldBounds.min;

  const mdl::BrushNode* result = nullptr;
  auto most = 0.0;
  for (const auto* brushNode : oldBrushes)
  {
    const auto bounds = brushNode->logicalBounds();
    if (sideOf(bounds, oldBounds) == side)
    {
      const auto volume = sharedVolume(bounds.translate(delta), box);
      if (!result || volume > most)
      {
        result = brushNode;
        most = volume;
      }
    }
  }
  return result;
}

/**
 * Replaces the carrier's brushes with a brush for each box, which takes the materials
 * of the old brush on the same side of the carrier that it overlaps most (see
 * materialSource), sets its keys as `setKeys` does and `_tbcli_fp` to the fingerprint
 * of the boxes. The carrier is reported as modified and its old brushes as removed.
 */
void rebuildCarrier(
  OpContext& context,
  const mdl::EntityNode& constCarrier,
  const std::vector<vm::bbox3d>& boxes,
  const std::function<void(mdl::Entity&)>& setKeys)
{
  auto& map = context.map;
  // the carriers were collected from the map that this op edits
  auto& carrier = const_cast<mdl::EntityNode&>(constCarrier);
  const auto what = describeCarrier(carrier);
  const auto builder = brushBuilderOf(map);

  const auto oldNodes = carrier.children();
  auto oldBrushes = std::vector<const mdl::BrushNode*>{};
  for (const auto* child : oldNodes)
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
    {
      oldBrushes.push_back(brushNode);
    }
  }
  const auto oldBounds = carrier.logicalBounds();
  auto newBounds = boxes.front();
  for (const auto& box : boxes)
  {
    newBounds = vm::merge(newBounds, box);
  }

  auto pieces = std::vector<std::unique_ptr<mdl::BrushNode>>{};
  for (const auto& box : boxes)
  {
    const auto* source = materialSource(box, newBounds, oldBrushes, oldBounds);
    pieces.push_back(
      std::make_unique<mdl::BrushNode>(
        cuboidLike(context, builder, box, source ? &source->brush() : nullptr)));
  }
  auto nodes =
    pieces
    | std::views::transform([](const auto& piece) -> mdl::Node* { return piece.get(); })
    | kdl::ranges::to<std::vector>();
  if (mdl::addNodes(map, {{&carrier, std::move(nodes)}}).empty())
  {
    throw opFailed(context.opName, fmt::format("cannot rebuild {}", what));
  }
  for (auto& piece : pieces)
  {
    piece.release();
  }
  // the new brushes are added first, so that removing the old ones never empties the
  // carrier
  mdl::removeNodes(map, oldNodes);

  auto entity = carrier.entity();
  setKeys(entity);
  entity.addOrUpdateProperty(Keys::Fingerprint, boxesFingerprint(boxes));
  updateCarrierKeys(context, carrier, std::move(entity), what);

  context.outcome.modified.push_back(&carrier);
  std::ranges::copy(oldNodes, std::back_inserter(context.outcome.removed));
}

/**
 * What a rebuild replaces: the rooms and connections whose brushes it rebuilds with
 * their new parameters, and where the rooms stand before and after it.
 */
struct RebuildPlan
{
  /** The rooms and connections, and where the rooms stand before the rebuild. */
  Layout layout;
  /** Where the rooms stand after the rebuild. */
  Shapes after;
  /** The rooms whose brushes are rebuilt, in order, with their new parameters. */
  std::vector<std::pair<const BuiltRoom*, RoomParams>> roomsToRebuild;
  /** The connections whose brushes are rebuilt, in order, with their new parameters. */
  std::vector<std::pair<const BuiltConnection*, ConnectionParams>> connectionsToRebuild;
  /** The rooms left where they are because their placement could not be matched. */
  std::vector<const BuiltRoom*> stale;

  explicit RebuildPlan(const mdl::WorldNode& worldNode)
    : layout{layoutOf(worldNode)}
    , after{layout.shapes}
  {
  }

  // the plan points into the rooms and connections of its layout
  RebuildPlan(const RebuildPlan&) = delete;
  RebuildPlan& operator=(const RebuildPlan&) = delete;

  bool rebuildsRoom(const BuiltRoom& room) const
  {
    return std::ranges::any_of(
      roomsToRebuild, [&](const auto& entry) { return entry.first == &room; });
  }

  /** Adds the room with the given parameters, unless it is rebuilt already. */
  void rebuildRoom(const BuiltRoom& room, const RoomParams& params)
  {
    if (!rebuildsRoom(room))
    {
      roomsToRebuild.emplace_back(&room, params);
    }
  }

  /** The parameters that the connection is rebuilt with, if it is rebuilt. */
  const ConnectionParams* rebuiltParams(const BuiltConnection& connection) const
  {
    const auto it = std::ranges::find(
      connectionsToRebuild, &connection, [](const auto& entry) { return entry.first; });
    return it != connectionsToRebuild.end() ? &it->second : nullptr;
  }
};

/** What rebuilding a connection does with its rooms, for messages. */
std::string rebuildAction(const BuiltConnection& connection)
{
  return fmt::format("rebuild connection {} with", connection.name());
}

/**
 * Adds a connection with the given parameters to the rebuild, unless it is rebuilt
 * already, and the rooms that it joins, whose walls it cuts.
 *
 * @throw CliError as findRoom does if a room that it joins is missing, ambiguous or
 * malformed
 */
void rebuildConnection(
  const std::string_view opName,
  RebuildPlan& plan,
  const BuiltConnection& connection,
  const ConnectionParams& params)
{
  if (plan.rebuiltParams(connection))
  {
    return;
  }
  plan.connectionsToRebuild.emplace_back(&connection, params);
  for (const auto& name : {connection.from, connection.to})
  {
    const auto& room =
      findRoom(opName, rebuildAction(connection), plan.layout.rooms, name);
    plan.rebuildRoom(room, *room.params);
  }
}

/** Whether the room is placed against the reference, directly or through other rooms. */
bool isPlacedAgainst(
  const std::vector<BuiltRoom>& rooms, const BuiltRoom& room, const BuiltRoom& reference)
{
  auto names = std::vector<std::string>{reference.name};
  for (size_t i = 0; i < names.size(); ++i)
  {
    for (const auto& each : rooms)
    {
      if (
        each.params && each.params->place && each.params->place->room == names[i]
        && !kdl::ranges::contains(names, each.name))
      {
        if (&each == &room)
        {
          return true;
        }
        names.push_back(each.name);
      }
    }
  }
  return false;
}

/**
 * Re-places the rooms that are placed against the given moved rooms, directly or
 * through others, and adds the ones that move and every connection that joins a moved
 * room to the rebuild. A room that does not stand where its placement puts it, or
 * whose reference's name is ambiguous, stays where it is and is recorded as stale.
 */
void moveDependents(
  const std::string_view opName, RebuildPlan& plan, std::vector<const BuiltRoom*> moved)
{
  for (size_t i = 0; i < moved.size(); ++i)
  {
    const auto& reference = *moved[i];
    const auto& referenceBefore = plan.layout.shapes.at(reference.carrier).interior;
    const auto& referenceAfter = plan.after.at(reference.carrier).interior;
    if (vm::is_equal(referenceBefore, referenceAfter, OverlapTolerance))
    {
      continue;
    }

    for (const auto& room : plan.layout.rooms)
    {
      if (
        !room.params || !room.params->place || room.params->place->room != reference.name
        || !plan.layout.shapes.contains(room.carrier)
        || kdl::ranges::contains(moved, &room))
      {
        continue;
      }

      const auto& before = plan.layout.shapes.at(room.carrier).interior;
      const auto interior = uniqueRoom(plan.layout.rooms, reference.name) == &reference
                              ? replacedInterior(
                                  before,
                                  room.params->size,
                                  *room.params->place,
                                  referenceBefore,
                                  referenceAfter)
                              : std::nullopt;
      if (!interior)
      {
        plan.stale.push_back(&room);
      }
      else if (!vm::is_equal(*interior, before, OverlapTolerance))
      {
        plan.after.at(room.carrier).interior = *interior;
        plan.rebuildRoom(room, *room.params);
        moved.push_back(&room);
      }
    }
  }

  for (const auto& connection : plan.layout.connections)
  {
    if (std::ranges::any_of(moved, [&](const auto* room) {
          return connection.joins(room->name);
        }))
    {
      rebuildConnection(opName, plan, connection, paramsOf(opName, connection));
    }
  }
}

/**
 * A carrier that a rebuild replaces the brushes of: the boxes of its new brushes and how
 * to set its keys.
 */
struct RebuiltCarrier
{
  const mdl::EntityNode* carrier = nullptr;
  std::vector<vm::bbox3d> boxes;
  std::function<void(mdl::Entity&)> setKeys;
  /** The space that its brushes take after the rebuild. */
  vm::bbox3d shell;
};

/**
 * Carries out a rebuild: replaces the brushes of every room and connection of the plan
 * with what their parameters build where the rooms stand after it, and reports the
 * stale placements.
 *
 * @throw CliError `locked` if a carrier is locked, without --allow-locked,
 * `hand_edited` if a carrier's brushes no longer match what its parameters build,
 * without `force`, `connect_cuts_linked_room` if the walls of a room inside a linked
 * group would be cut, the layout errors of corridorBetween, `layout_overlap` if a
 * rebuilt room or corridor would overlap another, and as findRoom and paramsOf do if a
 * connection of a rebuilt room cannot be built
 */
void executeRebuild(OpContext& context, const RebuildPlan& plan, const bool force)
{
  const auto opName = context.opName;
  const auto forceHint =
    "pass force to rebuild it anyway, or detach it to keep its brushes";
  for (const auto& [room, params] : plan.roomsToRebuild)
  {
    checkUnlocked(context, *room->carrier, describeCarrier(*room->carrier), "rebuild it");
    if (!force && room->status != RoomStatus::Rebuildable)
    {
      throw handEdited(context, *room->carrier, forceHint);
    }
  }
  for (const auto& [connection, params] : plan.connectionsToRebuild)
  {
    checkUnlocked(
      context, *connection->carrier, describeCarrier(*connection->carrier), "rebuild it");
    if (!force && connectionStatus(*connection, plan.layout) != RoomStatus::Rebuildable)
    {
      throw handEdited(context, *connection->carrier, forceHint);
    }
  }

  const auto corridorAfter = [&](const BuiltConnection& connection) {
    const auto* rebuilt = plan.rebuiltParams(connection);
    const auto& params = rebuilt ? *rebuilt : paramsOf(opName, connection);
    const auto action = rebuildAction(connection);
    const auto& from = findRoom(opName, action, plan.layout.rooms, connection.from);
    const auto& to = findRoom(opName, action, plan.layout.rooms, connection.to);
    return corridorBetween(
      opName, plan.after.at(from.carrier), plan.after.at(to.carrier), params);
  };

  auto rebuilt = std::vector<RebuiltCarrier>{};
  for (const auto& [room, params] : plan.roomsToRebuild)
  {
    if (std::ranges::any_of(plan.layout.connections, [&](const auto& connection) {
          return connection.joins(room->name);
        }))
    {
      checkNotLinked(context, *room);
    }
    // corridorAfter throws rather than build no corridor
    const auto openings = *openingsOf(
      room->name, plan.layout.connections, [&](const BuiltConnection& connection) {
        return std::optional{corridorAfter(connection)};
      });
    const auto& shape = plan.after.at(room->carrier);
    rebuilt.push_back(
      RebuiltCarrier{
        room->carrier,
        roomBoxes(shape, openings),
        [params](mdl::Entity& entity) {
          entity.addOrUpdateProperty(Keys::Size, formatVec3(params.size));
          entity.addOrUpdateProperty(Keys::Wall, formatNumber(params.wall));
          if (params.place)
          {
            entity.addOrUpdateProperty(Keys::Place, placementToString(*params.place));
          }
          else
          {
            entity.removeProperty(Keys::Place);
          }
        },
        shape.interior.expand(shape.wall)});
  }
  for (const auto& [connection, params] : plan.connectionsToRebuild)
  {
    const auto corridor = corridorAfter(*connection);
    rebuilt.push_back(
      RebuiltCarrier{
        connection->carrier,
        corridorBoxes(corridor),
        [params](mdl::Entity& entity) {
          entity.addOrUpdateProperty(Keys::Width, formatNumber(params.width));
          entity.addOrUpdateProperty(Keys::Height, formatNumber(params.height));
          entity.addOrUpdateProperty(Keys::Offset, formatNumber(params.offset));
        },
        corridorShell(corridor)});
  }

  const auto obstacles = carrierObstacles(
    context.map.worldNode(),
    rebuilt | std::views::transform(&RebuiltCarrier::carrier)
      | kdl::ranges::to<std::vector>());
  for (const auto& each : rebuilt)
  {
    auto others = obstacles;
    for (const auto& other : rebuilt)
    {
      if (&other != &each)
      {
        others.push_back({other.shell, other.carrier});
      }
    }
    checkNoOverlap(
      opName,
      fmt::format("the rebuilt {}", describeCarrier(*each.carrier)),
      each.shell,
      others,
      "change the parameters or move the rooms apart first");
  }

  for (const auto& each : rebuilt)
  {
    rebuildCarrier(context, *each.carrier, each.boxes, each.setKeys);
  }
  for (const auto* room : plan.stale)
  {
    context.outcome.stalePlacements.push_back(
      const_cast<mdl::EntityNode*>(room->carrier));
  }
  if (!plan.stale.empty())
  {
    context.outcome.warnings.emplace_back("stale_placement");
  }
}

/** Whether the name is two room names joined by a dash, as `A-B` names a connection. */
bool isConnectionName(const std::string_view name)
{
  const auto rooms = kdl::str_split(name, "-");
  return rooms.size() == 2 && isRoomName(rooms[0]) && isRoomName(rooms[1])
         && rooms[0] != rooms[1] && std::ranges::count(name, '-') == 1;
}

/**
 * Checks that the argument is a room name.
 *
 * @throw CliError `invalid_argument` if it is not made of letters, digits and
 * underscores
 */
void checkRoomNameArg(
  const std::string_view opName, const std::string_view argName, const Json& args)
{
  if (!isRoomName(args[argName].asString()))
  {
    throw invalidArgument(
      fmt::format(
        "{}: \"{}\" must be a room name, letters, digits and underscores, not {}",
        opName,
        argName,
        args[argName].dump()));
  }
}

/**
 * Checks that the argument names a connection.
 *
 * @throw CliError `invalid_argument` if it is not two different room names joined by a
 * dash
 */
void checkConnectionNameArg(
  const std::string_view opName, const std::string_view argName, const Json& args)
{
  if (!isConnectionName(args[argName].asString()))
  {
    throw invalidArgument(
      fmt::format(
        "{}: \"{}\" must name a connection by the rooms it joins, as \"A-B\", not {}",
        opName,
        argName,
        args[argName].dump()));
  }
}

/** Checks that the argument is greater than 0, if it is given. */
void checkPositiveArgIfGiven(
  const std::string_view opName, const std::string_view argName, const Json& args)
{
  if (args.find(argName))
  {
    checkPositiveArg(opName, argName, args);
  }
}

} // namespace

std::span<const std::string_view> relationNames()
{
  return RelationNames;
}

bool isPlacement(const Json& value)
{
  if (!value.isObject() || value.size() != 2)
  {
    return false;
  }
  const auto* gap = value.find("gap");
  return gap && gap->isNumber()
         && std::ranges::any_of(value.asObject(), [](const auto& member) {
              return parseRelation(member.key) && member.value.isString();
            });
}

Json paramsToJson(const RoomParams& params)
{
  auto result = Json::object();
  result.set("size", toJson(params.size));
  result.set("wall", params.wall);
  if (params.place)
  {
    result.set("place", placementToJson(*params.place));
  }
  return result;
}

std::string_view toString(const RoomStatus status)
{
  return status == RoomStatus::Rebuildable ? "rebuildable" : "hand_edited";
}

std::vector<BuiltRoom> collectBuiltRooms(const mdl::WorldNode& worldNode)
{
  return layoutOf(worldNode).rooms;
}

const BuiltRoom* builtRoomOf(const mdl::Node& node, const std::vector<BuiltRoom>& rooms)
{
  const auto it = std::ranges::find_if(rooms, [&](const auto& room) {
    return room.carrier == &node || room.group == &node;
  });
  return it != rooms.end() ? &*it : nullptr;
}

Json builtRoomToJson(const BuiltRoom& room)
{
  const auto groupPath = room.group ? std::optional{nodePath(*room.group)} : std::nullopt;

  auto instances = Json::array();
  instances.push(groupPath ? *groupPath : nodePath(*room.carrier));

  auto result = Json::object();
  result.set("name", room.name);
  result.set("kind", RoomKind);
  result.set("params", room.params ? paramsToJson(*room.params) : Json{});
  result.set("status", toString(room.status));
  result.set("instances", std::move(instances));
  result.set("group", groupPath ? Json{*groupPath} : Json{});
  return result;
}

void addRoom(OpContext& context)
{
  const auto& args = context.args;
  auto& worldNode = context.map.worldNode();
  auto& layer = defaultLayerToBuildIn(context);

  const auto& name = args["name"].asString();
  const auto rooms = collectBuiltRooms(worldNode);
  for (const auto& room : rooms)
  {
    if (room.name == name)
    {
      const auto path = nodePath(*room.carrier);
      throw opFailed(
        context.opName,
        fmt::format("a built room named \"{}\" exists already ({})", name, path),
        path);
    }
  }

  const auto shapes = shapesOf(rooms);
  auto params = RoomParams{vec3FromJson(args["size"]), args["wall"].asNumber(), {}};
  auto interior = vm::bbox3d{};
  if (const auto* place = args.find("place"))
  {
    params.place = placementFromJson(*place);
    const auto& reference =
      findRoom(context.opName, "place against", rooms, params.place->room);
    interior =
      placedInterior(shapes.at(reference.carrier).interior, params.size, *params.place);
  }
  else
  {
    const auto min = vec3FromJson(args["min"]);
    interior = vm::bbox3d{min, min + params.size};
  }

  checkNoOverlap(
    context.opName,
    fmt::format("the walls of room \"{}\"", name),
    interior.expand(params.wall),
    carrierObstacles(worldNode),
    "place it elsewhere or with a larger gap");

  auto interiors = interiorsOf(rooms, shapes);
  interiors.insert(interiors.begin(), interior);

  auto properties = std::vector<mdl::EntityProperty>{
    {mdl::EntityPropertyKeys::Classname, mdl::EntityPropertyValues::GroupClassname},
    {Keys::Kind, RoomKind},
    {Keys::Name, name},
    {Keys::Size, formatVec3(params.size)},
    {Keys::Wall, formatNumber(params.wall)},
  };
  if (params.place)
  {
    properties.emplace_back(Keys::Place, placementToString(*params.place));
  }
  addCarrierGroup(
    context,
    layer,
    newCarrierGroup(
      context,
      roomGroupName(name),
      std::move(properties),
      lowerRoom(interior, params.wall, interiors)));
}

void checkAddRoomArgs(const Json& args)
{
  if (!isRoomName(args["name"].asString()))
  {
    throw invalidArgument(
      fmt::format(
        "add_room: \"name\" must be letters, digits and underscores, not {}",
        args["name"].dump()));
  }

  const auto size = vec3FromJson(args["size"]);
  if (!(size.x() > 0 && size.y() > 0 && size.z() > 0))
  {
    throw invalidArgument(
      fmt::format(
        "add_room: \"size\" must be greater than 0 on every axis, not {}",
        args["size"].dump()));
  }
  checkPositiveArg("add_room", "wall", args);

  if ((args.find("min") != nullptr) == (args.find("place") != nullptr))
  {
    throw invalidArgument(
      "add_room: give exactly one of \"min\" (absolute) and \"place\" (next to another "
      "room)");
  }
}

void connect(OpContext& context)
{
  const auto& args = context.args;
  auto& worldNode = context.map.worldNode();
  auto& layer = defaultLayerToBuildIn(context);

  const auto& fromName = args["from"].asString();
  const auto& toName = args["to"].asString();
  const auto params = ConnectionParams{
    args["width"].asNumber(), args["height"].asNumber(), args["offset"].asNumber()};

  const auto rooms = collectBuiltRooms(worldNode);
  const auto& from = roomToConnect(context, rooms, fromName);
  const auto& to = roomToConnect(context, rooms, toName);
  checkNotConnected(context, fromName, toName);
  const auto shapes = shapesOf(rooms);
  const auto corridor = corridorBetween(
    context.opName, shapes.at(from.carrier), shapes.at(to.carrier), params);
  checkNoOverlap(
    context.opName,
    fmt::format("the corridor between rooms \"{}\" and \"{}\"", fromName, toName),
    corridorShell(corridor),
    carrierObstacles(worldNode),
    "move the rooms apart or the corridor along the wall");

  auto interiors = interiorsOf(rooms, shapes);
  interiors.insert(interiors.begin(), corridor.interior);
  auto group = newCarrierGroup(
    context,
    connectionGroupName(fromName, toName),
    {
      {mdl::EntityPropertyKeys::Classname, mdl::EntityPropertyValues::GroupClassname},
      {Keys::Kind, ConnectionKind},
      {Keys::From, fromName},
      {Keys::To, toName},
      {Keys::Width, formatNumber(params.width)},
      {Keys::Height, formatNumber(params.height)},
      {Keys::Offset, formatNumber(params.offset)},
    },
    lowerSlabs(corridorSlabs(corridor), interiors));

  cutOpening(context, from, corridor.interior);
  cutOpening(context, to, corridor.interior);
  addCarrierGroup(context, layer, std::move(group));
}

void checkConnectArgs(const Json& args)
{
  checkRoomNameArg("connect", "from", args);
  checkRoomNameArg("connect", "to", args);
  if (args["from"].asString() == args["to"].asString())
  {
    throw invalidArgument(
      fmt::format(
        "connect: \"from\" and \"to\" must name two rooms, not {} twice",
        args["from"].dump()));
  }
  checkPositiveArg("connect", "width", args);
  checkPositiveArg("connect", "height", args);
}

void editRoom(OpContext& context)
{
  const auto& args = context.args;
  const auto opName = context.opName;
  auto plan = RebuildPlan{context.map.worldNode()};
  const auto& room = findRoom(opName, "edit", plan.layout.rooms, args["room"].asString());

  auto params = *room.params;
  if (const auto* size = args.find("size"))
  {
    params.size = vec3FromJson(*size);
  }
  if (const auto* wall = args.find("wall"))
  {
    params.wall = wall->asNumber();
  }

  // a room that is not placed, or whose placement is stale, keeps its minimum
  const auto& before = plan.layout.shapes.at(room.carrier).interior;
  auto interior = vm::bbox3d{before.min, before.min + params.size};
  if (const auto* place = args.find("place"))
  {
    params.place = placementFromJson(*place);
    const auto& reference =
      findRoom(opName, "place against", plan.layout.rooms, params.place->room);
    if (&reference == &room || isPlacedAgainst(plan.layout.rooms, reference, room))
    {
      throw opFailed(
        opName,
        fmt::format(
          "cannot place room \"{}\" against room \"{}\", which is placed against it",
          room.name,
          reference.name),
        nodePath(*room.carrier));
    }
    interior = placedInterior(
      plan.layout.shapes.at(reference.carrier).interior, params.size, *params.place);
  }
  else if (params.place)
  {
    const auto* reference = uniqueRoom(plan.layout.rooms, params.place->room);
    const auto referenceShape =
      reference ? plan.layout.shapes.find(reference->carrier) : plan.layout.shapes.end();
    const auto replaced = referenceShape != plan.layout.shapes.end()
                            ? replacedInterior(
                                before,
                                params.size,
                                *params.place,
                                referenceShape->second.interior,
                                referenceShape->second.interior)
                            : std::nullopt;
    if (replaced)
    {
      interior = *replaced;
    }
    else
    {
      plan.stale.push_back(&room);
    }
  }

  plan.after.at(room.carrier) = RoomShape{room.name, interior, params.wall};
  plan.rebuildRoom(room, params);
  moveDependents(opName, plan, {&room});
  executeRebuild(context, plan, args["force"].asBool());
}

void checkEditRoomArgs(const Json& args)
{
  checkRoomNameArg("edit_room", "room", args);
  if (const auto* size = args.find("size"))
  {
    const auto value = vec3FromJson(*size);
    if (!(value.x() > 0 && value.y() > 0 && value.z() > 0))
    {
      throw invalidArgument(
        fmt::format(
          "edit_room: \"size\" must be greater than 0 on every axis, not {}",
          size->dump()));
    }
  }
  checkPositiveArgIfGiven("edit_room", "wall", args);
}

void editConnection(OpContext& context)
{
  const auto& args = context.args;
  const auto opName = context.opName;
  auto plan = RebuildPlan{context.map.worldNode()};
  const auto& connection = findConnection(
    opName, "edit", plan.layout.connections, args["connection"].asString());

  auto params = paramsOf(opName, connection);
  if (const auto* width = args.find("width"))
  {
    params.width = width->asNumber();
  }
  if (const auto* height = args.find("height"))
  {
    params.height = height->asNumber();
  }
  if (const auto* offset = args.find("offset"))
  {
    params.offset = offset->asNumber();
  }

  rebuildConnection(opName, plan, connection, params);
  executeRebuild(context, plan, args["force"].asBool());
}

void checkEditConnectionArgs(const Json& args)
{
  checkConnectionNameArg("edit_connection", "connection", args);
  checkPositiveArgIfGiven("edit_connection", "width", args);
  checkPositiveArgIfGiven("edit_connection", "height", args);
}

void detach(OpContext& context)
{
  const auto& args = context.args;
  const auto& worldNode = context.map.worldNode();
  const auto rooms = collectBuiltRooms(worldNode);
  const auto connections = collectConnections(worldNode);
  const auto* room = args.find("room");
  const auto* carrier =
    room ? findNamedRoom(context.opName, "detach", rooms, room->asString()).carrier
         : findConnection(
             context.opName, "detach", connections, args["connection"].asString())
             .carrier;

  const auto what = describeCarrier(*carrier);
  checkUnlocked(context, *carrier, what, "detach it");

  auto entity = carrier->entity();
  auto properties = std::vector<mdl::EntityProperty>{};
  std::ranges::copy_if(
    entity.properties(), std::back_inserter(properties), [](const auto& property) {
      return !property.key().starts_with(Keys::Prefix);
    });
  entity.setProperties(std::move(properties));

  // the carrier was collected from the map that this op edits
  auto& mutableCarrier = const_cast<mdl::EntityNode&>(*carrier);
  updateCarrierKeys(context, mutableCarrier, std::move(entity), what);
  context.outcome.modified.push_back(&mutableCarrier);
}

void checkDetachArgs(const Json& args)
{
  const auto* room = args.find("room");
  const auto* connection = args.find("connection");
  if ((room != nullptr) == (connection != nullptr))
  {
    throw invalidArgument("detach: give exactly one of \"room\" and \"connection\"");
  }
  if (room)
  {
    checkRoomNameArg("detach", "room", args);
  }
  else
  {
    checkConnectionNameArg("detach", "connection", args);
  }
}

} // namespace tb::cli
