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
#include "kd/ranges/to.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <ranges>
#include <utility>

namespace tb::cli
{
namespace
{

namespace Keys
{
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

/**
 * Whether the carrier's parameters are well-formed and its brushes still have the
 * fingerprint of the brushes that add_room built. Comparing with what the parameters
 * build now is left to rebuilds, since connections cut the walls that a room's
 * parameters alone build.
 */
RoomStatus statusOf(
  const mdl::EntityNode& carrier, const std::optional<RoomParams>& params)
{
  const auto* stored = carrier.entity().property(Keys::Fingerprint);
  return params && carrier.hasChildren() && stored
             && *stored == carrierFingerprint(carrier)
           ? RoomStatus::Rebuildable
           : RoomStatus::HandEdited;
}

/** The carrier of a built room that the node is, if it is one. */
const mdl::EntityNode* carrierOf(const mdl::Node& node)
{
  const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(&node);
  return entityNode && isRoomCarrier(*entityNode) ? entityNode : nullptr;
}

std::string roomGroupName(const std::string_view name)
{
  return fmt::format("{}{}", RoomGroupPrefix, name);
}

BuiltRoom builtRoomOfCarrier(const mdl::EntityNode& carrier)
{
  const auto& entity = carrier.entity();
  auto name = *entity.property(Keys::Name);
  auto params = parseRoomParams(entity);
  const auto status = statusOf(carrier, params);

  const auto* group = dynamic_cast<const mdl::GroupNode*>(carrier.parent());
  if (group && group->name() != roomGroupName(name))
  {
    group = nullptr;
  }
  return BuiltRoom{&carrier, std::move(name), std::move(params), status, group};
}

Json placementToJson(const Placement& place)
{
  auto result = Json::object();
  result.set(std::string{toString(place.relation)}, place.room);
  result.set("gap", place.gap);
  return result;
}

/** The interior of a room: the bounds of its carrier's brushes less its walls. */
vm::bbox3d interiorOf(const BuiltRoom& room)
{
  return room.carrier->logicalBounds().expand(-room.params->wall);
}

/**
 * The built room with the given name that an op acts on.
 *
 * @param action what the op does with the room, for messages, as in `place against`
 */
const BuiltRoom& findRoom(
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
    auto paths = std::vector<std::string>{};
    for (const auto* room : candidates)
    {
      paths.push_back(nodePath(*room->carrier));
    }
    throw opFailed(
      opName,
      fmt::format(
        "{} built rooms are named \"{}\" ({}); rename or delete all but one",
        candidates.size(),
        name,
        kdl::str_join(paths, ", ")));
  }

  const auto& room = *candidates.front();
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

/**
 * The interior of a room of the given size placed next to the reference room: on the
 * given side at the gap, centred on the other horizontal axis, at the same floor
 * height.
 */
vm::bbox3d placedInterior(
  const vm::bbox3d& reference, const vm::vec3d& size, const Placement& place)
{
  const auto axis =
    place.relation == Relation::EastOf || place.relation == Relation::WestOf ? size_t{0}
                                                                             : size_t{1};
  const auto positive =
    place.relation == Relation::EastOf || place.relation == Relation::NorthOf;
  const auto other = 1 - axis;

  auto min = vm::vec3d{0, 0, reference.min.z()};
  min[axis] = positive ? reference.max[axis] + place.gap
                       : reference.min[axis] - place.gap - size[axis];
  // rounded half up, as in the reference lowering
  min[other] =
    reference.min[other] + std::floor((reference.size()[other] - size[other]) / 2 + 0.5);
  return {min, min + size};
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

/**
 * Fails if the box overlaps the brushes of a built room or a connection by more than
 * OverlapTolerance, compared by the bounds of their carriers. Boxes that only touch do
 * not overlap; carriers without brushes are skipped.
 *
 * @param what what would overlap, for the message, as in `the walls of room "B"`
 * @param hint how to avoid the overlap, for the message
 */
void checkNoOverlap(
  const mdl::WorldNode& worldNode,
  const std::string_view opName,
  const std::string_view what,
  const vm::bbox3d& box,
  const std::string_view hint)
{
  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (
      (isRoomCarrier(*entityNode) || isConnectionCarrier(*entityNode))
      && entityNode->hasChildren()
      && overlaps(box, entityNode->logicalBounds(), OverlapTolerance))
    {
      const auto path = nodePath(*entityNode);
      throw CliError{
        ErrorCode::LayoutOverlap,
        fmt::format(
          "{}: {} would overlap {} ({}); {}",
          opName,
          what,
          describeCarrier(*entityNode),
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

/** The interiors of the rooms that have parameters and brushes. */
std::vector<vm::bbox3d> roomInteriors(const std::vector<BuiltRoom>& rooms)
{
  auto result = std::vector<vm::bbox3d>{};
  for (const auto& room : rooms)
  {
    if (room.params && room.carrier->hasChildren())
    {
      result.push_back(interiorOf(room));
    }
  }
  return result;
}

/**
 * A new carrier with the given keys that holds a brush for each box, in a new group with
 * the given name. `_tbcli_fp` is added to the keys: the fingerprint of the boxes, which
 * rebuilds compare against.
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
 * half up. Its walls are as thick as the thicker of the rooms' walls.
 *
 * @throw CliError `layout_corridor_does_not_fit` if the rooms do not stand side by side,
 * their walls touch so that the corridor would have no length, or the corridor reaches
 * beyond the wall that they share, `layout_floor_mismatch` if their floors are at
 * different heights, and `layout_corridor_too_tall` if the corridor would reach above a
 * room's ceiling
 */
Corridor corridorBetween(
  const std::string_view opName,
  const BuiltRoom& from,
  const BuiltRoom& to,
  const ConnectionParams& params)
{
  const auto fromInterior = interiorOf(from);
  const auto toInterior = interiorOf(to);

  const auto axes = corridorAxesBetween(fromInterior, toInterior);
  if (!axes)
  {
    throw CliError{
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
    throw CliError{
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
    std::floor((sharedMin + sharedMax) / 2 + params.offset - params.width / 2 + 0.5);
  const auto acrossMax = acrossMin + params.width;
  if (
    acrossMin < sharedMin - OverlapTolerance || acrossMax > sharedMax + OverlapTolerance)
  {
    throw CliError{
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
    throw CliError{
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
  const auto firstInterior = interiorOf(*first);
  const auto secondInterior = interiorOf(*second);

  auto interior = vm::bbox3d{};
  interior.min[along] = firstInterior.max[along];
  interior.max[along] = secondInterior.min[along];
  interior.min[across] = acrossMin;
  interior.max[across] = acrossMax;
  interior.min[2] = floor;
  interior.max[2] = top;

  auto segment = interior;
  segment.min[along] += first->params->wall;
  segment.max[along] -= second->params->wall;
  if (segment.max[along] - segment.min[along] <= OverlapTolerance)
  {
    throw CliError{
      ErrorCode::LayoutCorridorDoesNotFit,
      fmt::format(
        "{}: the walls of rooms \"{}\" and \"{}\" touch, so a corridor between them "
        "would have no length; place them further apart",
        opName,
        from.name,
        to.name)};
  }

  return Corridor{interior, segment, *axes, std::max(from.params->wall, to.params->wall)};
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
 * A cuboid brush that fills the box, with the materials and UV of the faces of the
 * given brush that face the same way.
 */
mdl::Brush cuboidLike(
  const OpContext& context,
  const mdl::BrushBuilder& builder,
  const vm::bbox3d& box,
  const mdl::Brush& original)
{
  auto brush = builder.createCuboid(box, mdl::BrushFace::NoMaterialName);
  if (!brush)
  {
    throw opFailed(
      context.opName,
      fmt::format("cannot cut a wall: {}", std::get<Error>(brush.error()).msg));
  }
  auto result = std::move(brush).value();
  for (auto& face : result.faces())
  {
    if (const auto index = original.findFace(face.normal()))
    {
      const auto& originalFace = original.face(*index);
      if (const auto snapshot = originalFace.takeUvCoordSystemSnapshot())
      {
        face.restoreUvCoordSystemSnapshot(*snapshot);
      }
      face.copyAttributes(originalFace);
    }
  }
  return result;
}

/**
 * Cuts the opening out of a room's walls: each brush that reaches into it is replaced by
 * the boxes of it that lie outside, which keep its materials. The room's fingerprint is
 * set to its cut brushes, so that it stays rebuildable; the fingerprint does not depend
 * on how the walls are cut into brushes, so the order in which openings are cut does
 * not matter. The carrier is reported as modified and the brushes that were cut as
 * removed.
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
              cuboidLike(context, builder, box, brushNode->brush())));
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
  if (!mdl::updateNodeContents(
        map,
        std::string{context.opName},
        {{&carrier, mdl::NodeContents{std::move(entity)}}},
        mdl::collectContainingGroups({&carrier})))
  {
    throw opFailed(
      context.opName,
      fmt::format("cannot update the fingerprint of room \"{}\"", room.name));
  }

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
 * A room whose walls a connection cuts.
 *
 * @throw CliError as findRoom does, `locked` if the room is locked, without
 * --allow-locked, `connect_cuts_linked_room` if it is inside a linked group, and
 * `hand_edited` if its brushes no longer match what tbcli built
 */
const BuiltRoom& roomToConnect(
  OpContext& context, const std::vector<BuiltRoom>& rooms, const std::string& name)
{
  const auto& room = findRoom(context.opName, "connect", rooms, name);
  const auto what = fmt::format("room \"{}\"", name);
  checkUnlocked(context, *room.carrier, what, "cut its walls");

  const auto path = nodePath(*room.carrier);
  if (isInLinkedGroup(context, *room.carrier))
  {
    throw CliError{
      ErrorCode::ConnectCutsLinkedRoom,
      fmt::format(
        "{}: {} ({}) is inside a linked group, and cutting its walls would cut every "
        "linked copy without a corridor",
        context.opName,
        what,
        path),
      path};
  }
  if (room.status != RoomStatus::Rebuildable)
  {
    throw CliError{
      ErrorCode::HandEdited,
      fmt::format(
        "{}: {} ({}) is hand_edited: its brushes no longer match what tbcli built, so "
        "its walls cannot be cut safely",
        context.opName,
        what,
        path),
      path};
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
  for (const auto* entityNode : collectContents(context.map.worldNode()).entities)
  {
    if (!isConnectionCarrier(*entityNode))
    {
      continue;
    }
    const auto& entity = entityNode->entity();
    const auto* existingFrom = entity.property(Keys::From);
    const auto* existingTo = entity.property(Keys::To);
    if (
      existingFrom && existingTo
      && ((*existingFrom == from && *existingTo == to) || (*existingFrom == to && *existingTo == from)))
    {
      const auto path = nodePath(*entityNode);
      throw opFailed(
        context.opName,
        fmt::format(
          "rooms \"{}\" and \"{}\" are joined already by {} ({})",
          from,
          to,
          describeCarrier(*entityNode),
          path),
        path);
    }
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
  auto result = std::vector<BuiltRoom>{};
  for (const auto* entityNode : collectContents(worldNode).entities)
  {
    if (isRoomCarrier(*entityNode))
    {
      result.push_back(builtRoomOfCarrier(*entityNode));
    }
  }
  return result;
}

std::optional<BuiltRoom> builtRoomOf(const mdl::Node& node)
{
  if (const auto* carrier = carrierOf(node))
  {
    return builtRoomOfCarrier(*carrier);
  }
  if (dynamic_cast<const mdl::GroupNode*>(&node))
  {
    for (const auto* child : node.children())
    {
      if (const auto* carrier = carrierOf(*child))
      {
        auto room = builtRoomOfCarrier(*carrier);
        if (room.group == &node)
        {
          return room;
        }
      }
    }
  }
  return std::nullopt;
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

  auto params = RoomParams{vec3FromJson(args["size"]), args["wall"].asNumber(), {}};
  auto interior = vm::bbox3d{};
  if (const auto* place = args.find("place"))
  {
    params.place = placementFromJson(*place);
    const auto& reference =
      findRoom(context.opName, "place against", rooms, params.place->room);
    interior = placedInterior(interiorOf(reference), params.size, *params.place);
  }
  else
  {
    const auto min = vec3FromJson(args["min"]);
    interior = vm::bbox3d{min, min + params.size};
  }

  checkNoOverlap(
    worldNode,
    context.opName,
    fmt::format("the walls of room \"{}\"", name),
    interior.expand(params.wall),
    "place it elsewhere or with a larger gap");

  auto interiors = roomInteriors(rooms);
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
  const auto corridor = corridorBetween(context.opName, from, to, params);
  checkNoOverlap(
    worldNode,
    context.opName,
    fmt::format("the corridor between rooms \"{}\" and \"{}\"", fromName, toName),
    corridorShell(corridor),
    "move the rooms apart or the corridor along the wall");

  auto interiors = roomInteriors(rooms);
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
  for (const auto* key : {"from", "to"})
  {
    if (!isRoomName(args[key].asString()))
    {
      throw invalidArgument(
        fmt::format(
          "connect: \"{}\" must be a room name, letters, digits and underscores, not {}",
          key,
          args[key].dump()));
    }
  }
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

} // namespace tb::cli
