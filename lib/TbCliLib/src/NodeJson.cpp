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

#include "cli/NodeJson.h"

#include "cli/EntityJson.h"
#include "cli/GeometryJson.h"
#include "cli/Json.h"
#include "cli/MapContents.h"
#include "cli/NodePath.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityNode.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/scalar.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>

namespace tb::cli
{
namespace
{

Json idToJson(const std::optional<mdl::IdType>& id)
{
  return id ? Json{*id} : Json{};
}

Json vec2ToJson(const vm::vec2f& v)
{
  return Json{Json::Array{double(v.x()), double(v.y())}};
}

/** Whether the given normal points along a coordinate axis. */
bool isAxial(const vm::vec3d& normal)
{
  return std::ranges::count_if(
           normal.v, [](const auto c) { return !vm::is_zero(c, vm::Cd::almost_zero()); })
         == 1;
}

/** The members that layers and groups share. */
Json layerOrGroupToJson(const mdl::Node& node, const std::optional<mdl::IdType>& id)
{
  const auto summary = summarize(collectContents(node));

  auto result = Json::object();
  result.set("path", nodePath(node));
  result.set("id", idToJson(id));
  result.set("name", node.name());
  result.set("bounds", boundsToJson(summary));
  result.set("counts", countsToJson(summary));
  // effective states, inherited from the parent unless set on the node
  result.set("hidden", !node.visible());
  result.set("locked", node.locked());
  return result;
}

Json pointsToJson(const auto& points)
{
  auto result = Json::array();
  for (const auto& point : points)
  {
    result.push(toJson(point));
  }
  return result;
}

} // namespace

bool isAxialBox(const mdl::Brush& brush)
{
  return brush.faceCount() == 6
         && std::ranges::all_of(
           brush.faces(), [](const auto& face) { return isAxial(face.normal()); });
}

Json countsToJson(const Summary& summary)
{
  auto counts = Json::object();
  counts.set("brushes", summary.brushes);
  counts.set("entities", summary.entities);
  counts.set("faces", summary.faces);
  return counts;
}

Json boundsToJson(const Summary& summary)
{
  return summary.bounds ? toJson(*summary.bounds) : Json{};
}

Json layerToJson(const mdl::LayerNode& layerNode)
{
  auto result = layerOrGroupToJson(layerNode, layerNode.persistentId());
  result.set("omit_from_export", layerNode.layer().omitFromExport());
  return result;
}

Json groupToJson(const mdl::GroupNode& groupNode)
{
  return layerOrGroupToJson(groupNode, groupNode.persistentId());
}

Json brushSummaryToJson(const mdl::BrushNode& brushNode)
{
  const auto& brush = brushNode.brush();

  auto textureNames = std::set<std::string>{};
  for (const auto& face : brush.faces())
  {
    textureNames.insert(face.materialName());
  }
  auto textures = Json::array();
  for (const auto& name : textureNames)
  {
    textures.push(name);
  }

  auto result = Json::object();
  result.set("path", nodePath(brushNode));
  result.set("aabb", toJson(brushNode.logicalBounds()));
  result.set("axial_box", isAxialBox(brush));
  result.set("textures", std::move(textures));
  result.set(
    "entity",
    brushNode.entity() ? nodePath(*brushNode.entity()) : std::string{WorldPath});
  return result;
}

Json brushToJson(const mdl::BrushNode& brushNode, const bool raw)
{
  const auto& brush = brushNode.brush();

  auto faces = Json::array();
  for (size_t i = 0; i < brush.faceCount(); ++i)
  {
    faces.push(faceToJson(brushNode, i, raw));
  }

  auto result = brushSummaryToJson(brushNode);
  result.set("vertices", pointsToJson(brush.vertexPositions()));
  result.set("faces", std::move(faces));
  return result;
}

Json faceSummaryToJson(const mdl::BrushNode& brushNode, const size_t faceIndex)
{
  const auto& face = brushNode.brush().face(faceIndex);

  auto result = Json::object();
  result.set("path", facePath(brushNode, faceIndex));
  result.set("normal", toJson(face.normal()));
  result.set("texture", face.materialName());
  return result;
}

Json faceToJson(const mdl::BrushNode& brushNode, const size_t faceIndex, const bool raw)
{
  const auto& face = brushNode.brush().face(faceIndex);
  const auto uvAttributes = face.uvAttributes();

  auto uv = Json::object();
  uv.set("u_axis", toJson(face.uAxis()));
  uv.set("v_axis", toJson(face.vAxis()));
  uv.set("offset", vec2ToJson(uvAttributes.offset));
  uv.set("scale", vec2ToJson(uvAttributes.scale));
  uv.set("rotation", double(uvAttributes.rotation));

  auto result = faceSummaryToJson(brushNode, faceIndex);
  result.set("uv", std::move(uv));
  result.set("vertices", pointsToJson(face.vertexPositions()));
  if (raw)
  {
    result.set("points", pointsToJson(face.points()));
  }
  return result;
}

Json patchSummaryToJson(const mdl::PatchNode& patchNode)
{
  auto result = Json::object();
  result.set("path", nodePath(patchNode));
  result.set("aabb", toJson(patchNode.logicalBounds()));
  result.set("texture", patchNode.patch().materialName());
  return result;
}

Json nodeSummaryToJson(const mdl::Node& node)
{
  return node.accept(
    kdl::overload(
      [](const mdl::WorldNode& worldNode) { return boundedEntityToJson(worldNode); },
      [](const mdl::LayerNode& layerNode) { return layerToJson(layerNode); },
      [](const mdl::GroupNode& groupNode) { return groupToJson(groupNode); },
      [](const mdl::EntityNode& entityNode) { return boundedEntityToJson(entityNode); },
      [](const mdl::BrushNode& brushNode) { return brushSummaryToJson(brushNode); },
      [](const mdl::PatchNode& patchNode) { return patchSummaryToJson(patchNode); }));
}

} // namespace tb::cli
