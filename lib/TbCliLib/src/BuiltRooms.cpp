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
#include "cli/NodePath.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Sha256.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/Group.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/ranges/to.h"
#include "kd/string_utils.h"

#include "vm/bbox.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
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
const auto Fingerprint = std::string{"_tbcli_fp"};
} // namespace Keys

/** The `_tbcli_kind` of a built room's carrier. */
const auto RoomKind = std::string{"room"};

/** The prefix of the name of the group that wraps a built room's carrier. */
constexpr auto RoomGroupPrefix = std::string_view{"built_room:"};

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

/** Whether the entity is a built room's carrier. */
bool isRoomCarrier(const mdl::EntityNode& entityNode)
{
  const auto& entity = entityNode.entity();
  return entity.classname() == mdl::EntityPropertyValues::GroupClassname
         && !entity.hasProperty(mdl::EntityPropertyKeys::TbGroupType)
         && entity.hasProperty(Keys::Kind, RoomKind) && entity.property(Keys::Name);
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
 * The boxes of a room, as the reference lowering builds them: the shell slabs around its
 * interior, less the given interiors of every room, so that no slab reaches into a room.
 */
std::vector<vm::bbox3d> lowerRoom(
  const vm::bbox3d& interior, const double wall, const std::vector<vm::bbox3d>& interiors)
{
  auto result = std::vector<vm::bbox3d>{};
  for (const auto& slab : shellSlabs(interior, wall))
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

std::vector<vm::vec3d> cornersOf(const vm::bbox3d& box)
{
  auto result = std::vector<vm::vec3d>{};
  for (const auto x : {box.min.x(), box.max.x()})
  {
    for (const auto y : {box.min.y(), box.max.y()})
    {
      for (const auto z : {box.min.z(), box.max.z()})
      {
        result.emplace_back(x, y, z);
      }
    }
  }
  return result;
}

/**
 * A fingerprint of the geometry of some brushes, given by their vertices, that ignores
 * where they are, their order and the order of their vertices: a hash of the vertices
 * relative to the minimum of all of them, rounded to 3 decimals, sorted per brush, and of
 * the brushes sorted.
 */
std::string geometryFingerprint(const std::vector<std::vector<vm::vec3d>>& brushes)
{
  auto origin = std::optional<vm::vec3d>{};
  for (const auto& vertices : brushes)
  {
    for (const auto& vertex : vertices)
    {
      origin = origin ? vm::min(*origin, vertex) : vertex;
    }
  }

  auto brushStrings = std::vector<std::string>{};
  for (const auto& vertices : brushes)
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
  return sha256Hex(kdl::str_join(brushStrings, ";")).substr(0, FingerprintLength);
}

std::string boxesFingerprint(const std::vector<vm::bbox3d>& boxes)
{
  return geometryFingerprint(
    boxes | std::views::transform(cornersOf) | kdl::ranges::to<std::vector>());
}


/** The fingerprint of the carrier's brushes. */
std::string carrierFingerprint(const mdl::EntityNode& carrier)
{
  auto brushes = std::vector<std::vector<vm::vec3d>>{};
  for (const auto* child : carrier.children())
  {
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(child))
    {
      brushes.push_back(brushNode->brush().vertexPositions());
    }
  }
  return geometryFingerprint(brushes);
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

/** The room with the given name that a room is placed against. */
const BuiltRoom& placementReference(
  const std::vector<BuiltRoom>& rooms, const std::string& name)
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
        "add_room: there is no built room named \"{}\" to place against; describe "
        "lists the built rooms",
        name)};
  }
  if (candidates.size() > 1)
  {
    auto paths = std::vector<std::string>{};
    for (const auto* room : candidates)
    {
      paths.push_back(nodePath(*room->carrier));
    }
    throw opFailed(
      "add_room",
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
      "add_room",
      fmt::format(
        "cannot place against room \"{}\", whose parameter keys are missing or "
        "malformed",
        name),
      path);
  }
  if (!room.carrier->hasChildren())
  {
    throw opFailed(
      "add_room",
      fmt::format("cannot place against room \"{}\", which has no brushes", name),
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

void checkNoOverlap(
  const std::vector<BuiltRoom>& rooms, const std::string& name, const vm::bbox3d& shell)
{
  for (const auto& room : rooms)
  {
    if (
      room.carrier->hasChildren()
      && overlaps(shell, room.carrier->logicalBounds(), OverlapTolerance))
    {
      const auto path = nodePath(*room.carrier);
      throw CliError{
        ErrorCode::LayoutOverlap,
        fmt::format(
          "add_room: the walls of room \"{}\" would overlap those of room \"{}\" ({}); "
          "place it elsewhere or with a larger gap",
          name,
          room.name,
          path),
        path};
    }
  }
}

/** The carrier of a new room, holding its brushes, in a new group. */
std::unique_ptr<mdl::GroupNode> newRoomGroup(
  const mdl::Map& map,
  const std::string& name,
  const RoomParams& params,
  const std::vector<vm::bbox3d>& boxes)
{
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
  // the fingerprint of the brushes as built, which rebuilds compare against
  properties.emplace_back(Keys::Fingerprint, boxesFingerprint(boxes));

  const auto builder = brushBuilderOf(map);


  auto carrier = std::make_unique<mdl::EntityNode>(mdl::Entity{std::move(properties)});
  for (const auto& box : boxes)
  {
    auto brush = builder.createCuboid(box, mdl::BrushFace::NoMaterialName);
    if (!brush)
    {
      throw opFailed(
        "add_room",
        fmt::format(
          "cannot create the brushes of room \"{}\": {}",
          name,
          std::get<Error>(brush.error()).msg));
    }
    carrier->addChild(new mdl::BrushNode{std::move(brush).value()});
  }

  auto group = std::make_unique<mdl::GroupNode>(mdl::Group{roomGroupName(name)});
  group->addChild(carrier.release());
  return group;
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
  auto& map = context.map;
  auto& worldNode = map.worldNode();

  auto* layer = worldNode.defaultLayer();
  if (!context.allowLocked && layer->locked())
  {
    throw CliError{
      ErrorCode::Locked,
      fmt::format(
        "add_room: the default layer {}, which rooms are created in, is locked; pass "
        "--allow-locked to edit it anyway",
        nodePath(*layer)),
      nodePath(*layer)};
  }

  const auto& name = args["name"].asString();
  const auto rooms = collectBuiltRooms(worldNode);
  for (const auto& room : rooms)
  {
    if (room.name == name)
    {
      const auto path = nodePath(*room.carrier);
      throw opFailed(
        "add_room",
        fmt::format("a built room named \"{}\" exists already ({})", name, path),
        path);
    }
  }

  auto params = RoomParams{vec3FromJson(args["size"]), args["wall"].asNumber(), {}};
  auto interior = vm::bbox3d{};
  if (const auto* place = args.find("place"))
  {
    params.place = placementFromJson(*place);
    const auto& reference = placementReference(rooms, params.place->room);
    interior = placedInterior(interiorOf(reference), params.size, *params.place);
  }
  else
  {
    const auto min = vec3FromJson(args["min"]);
    interior = vm::bbox3d{min, min + params.size};
  }

  checkNoOverlap(rooms, name, interior.expand(params.wall));

  auto interiors = std::vector{interior};
  for (const auto& room : rooms)
  {
    if (room.params && room.carrier->hasChildren())
    {
      interiors.push_back(interiorOf(room));
    }
  }
  auto group =
    newRoomGroup(map, name, params, lowerRoom(interior, params.wall, interiors));
  if (mdl::addNodes(map, {{layer, {group.get()}}}).empty())
  {
    throw opFailed("add_room", "cannot add the room to the map");
  }
  context.outcome.created.push_back(group.release());
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

} // namespace tb::cli
