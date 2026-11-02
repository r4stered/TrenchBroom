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

#include "cli/FaceOps.h"

#include "cli/CliError.h"
#include "cli/CliValidators.h"
#include "cli/Json.h"
#include "cli/Ops.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Map.h"
#include "mdl/Map_Brushes.h"
#include "mdl/UpdateBrushFaceAttributes.h"

#include "kd/contracts.h"

#include "vm/scalar.h"
#include "vm/vec.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

constexpr auto AllFaces = std::string_view{"all"};

constexpr auto DirectionNames =
  std::array<std::string_view, 6>{"+x", "-x", "+y", "-y", "+z", "-z"};

/** The direction that a selector element names, if it names one. */
std::optional<vm::vec3d> directionOf(const Json& value)
{
  if (!value.isString())
  {
    return std::nullopt;
  }

  // the names are in the order of the axes, positive first
  const auto it = std::ranges::find(DirectionNames, value.asString());
  if (it == DirectionNames.end())
  {
    return std::nullopt;
  }
  const auto index = size_t(it - DirectionNames.begin());
  const auto axis = vm::vec3d::axis(index / 2);
  return index % 2 == 0 ? axis : -axis;
}

/** The cosine of the largest angle between a direction and a face normal it picks. */
const auto MinDirectionCosine = std::cos(vm::to_radians(0.5));

mdl::BrushNode& brushNodeOf(mdl::Node& node)
{
  auto* brushNode = dynamic_cast<mdl::BrushNode*>(&node);
  contract_assert(brushNode != nullptr);
  return *brushNode;
}

/** An `op_failed` error about the given face, which names it by its path. */
CliError faceError(
  const std::string_view opName,
  const mdl::BrushFaceHandle& face,
  const std::string_view message)
{
  const auto path = facePath(face.node(), face.faceIndex());
  return CliError{
    ErrorCode::OpFailed, fmt::format("{}: {} {}", opName, path, message), path};
}

/**
 * The face selector of a face op: the given one, or all faces. A selector only applies to
 * brush targets.
 *
 * @throw CliError `invalid_argument` if a selector is given and a target is a face
 */
FaceSelector faceSelectorOf(const OpContext& context)
{
  const auto* faces = context.args.find("faces");
  if (!faces)
  {
    return FaceSelector{};
  }

  if (std::ranges::any_of(context.targets("target"), [](const auto& target) {
        return target.faceIndex.has_value();
      }))
  {
    throw invalidArgument(
      fmt::format(
        "{}: \"faces\" picks faces of brush targets, but a target is a face path",
        context.opName));
  }
  return *parseFaceSelector(*faces);
}

/**
 * Applies `edit` to the faces that the op's target and selector pick, and reports the
 * brushes whose faces changed as modified.
 *
 * @param failure what failed if `edit` returns false, for the error message
 */
void editFaces(
  OpContext& context,
  const std::function<bool(mdl::Map&, const std::vector<mdl::BrushFaceHandle>&)>& edit,
  const std::string_view failure)
{
  const auto& targets = context.targets("target");
  const auto faces = selectFaces(targets, faceSelectorOf(context));
  if (faces.empty())
  {
    if (!targets.empty())
    {
      context.outcome.warnings.emplace_back("no_faces_selected");
    }
    return;
  }

  // the edit swaps the brushes' contents, but the nodes and face indices stay
  auto before = std::vector<mdl::BrushFace>{};
  for (const auto& face : faces)
  {
    before.push_back(face.face());
  }

  if (!edit(context.map, faces))
  {
    throw CliError{ErrorCode::OpFailed, fmt::format("{}: {}", context.opName, failure)};
  }

  auto& modified = context.outcome.modified;
  for (size_t i = 0; i < faces.size(); ++i)
  {
    auto* brushNode = &faces[i].node();
    if (
      faces[i].face() != before[i]
      && std::ranges::find(modified, brushNode) == modified.end())
    {
      modified.push_back(brushNode);
    }
  }
}

/** The update of a value that is given as a number or as `{"add": n}`. */
mdl::ValueOp valueOp(const Json& value)
{
  if (const auto* add = value.find("add"))
  {
    return mdl::AddValue{float(add->asNumber())};
  }
  return mdl::SetValue{float(value.asNumber())};
}

/** The values of a pair that is given as `[u, v]`, or as one number for both. */
std::pair<float, float> pairOf(const Json& value)
{
  return value.isNumber()
           ? std::pair{float(value.asNumber()), float(value.asNumber())}
           : std::pair{float(value[0].asNumber()), float(value[1].asNumber())};
}

/**
 * The updates of a pair that is given as `[u, v]` or one number for both, or as
 * `{"add": ...}` with either.
 */
std::pair<mdl::ValueOp, mdl::ValueOp> valueOps(const Json& value)
{
  if (const auto* add = value.find("add"))
  {
    const auto [u, v] = pairOf(*add);
    return {mdl::AddValue{u}, mdl::AddValue{v}};
  }
  const auto [u, v] = pairOf(value);
  return {mdl::SetValue{u}, mdl::SetValue{v}};
}

} // namespace

std::span<const std::string_view> faceDirectionNames()
{
  return DirectionNames;
}

std::optional<FaceSelector> parseFaceSelector(const Json& value)
{
  if (value.isString() && value.asString() == AllFaces)
  {
    return FaceSelector{};
  }

  auto directions = std::vector<vm::vec3d>{};
  const auto add = [&](const Json& element) {
    const auto direction = directionOf(element);
    if (direction)
    {
      directions.push_back(*direction);
    }
    return direction.has_value();
  };

  if (value.isArray())
  {
    if (value.size() == 0 || !std::ranges::all_of(value.asArray(), add))
    {
      return std::nullopt;
    }
  }
  else if (!add(value))
  {
    return std::nullopt;
  }
  return FaceSelector{std::move(directions)};
}

bool isFaceSelector(const Json& value)
{
  return parseFaceSelector(value).has_value();
}

std::vector<mdl::BrushFaceHandle> selectFaces(
  const std::vector<NodeOrFace>& targets, const FaceSelector& selector)
{
  const auto& directions = selector.directions;
  const auto picks = [&](const mdl::BrushFace& face) {
    return !directions || std::ranges::any_of(*directions, [&](const auto& direction) {
      return vm::dot(face.normal(), direction) >= MinDirectionCosine;
    });
  };

  auto result = std::vector<mdl::BrushFaceHandle>{};
  auto seen = std::set<std::pair<const mdl::BrushNode*, size_t>>{};
  const auto add = [&](mdl::BrushNode& brushNode, const size_t faceIndex) {
    if (seen.emplace(&brushNode, faceIndex).second)
    {
      result.emplace_back(brushNode, faceIndex);
    }
  };

  for (const auto& [node, faceIndex] : targets)
  {
    auto& brushNode = brushNodeOf(*node);
    if (faceIndex)
    {
      add(brushNode, *faceIndex);
      continue;
    }

    const auto& brush = brushNode.brush();
    for (size_t i = 0; i < brush.faceCount(); ++i)
    {
      if (picks(brush.face(i)))
      {
        add(brushNode, i);
      }
    }
  }
  return result;
}

void setMaterial(OpContext& context)
{
  const auto& material = context.args["material"].asString();
  editFaces(
    context,
    [&](auto& map, const auto& faces) {
      if (!mdl::setBrushFaceAttributes(map, faces, {.materialName = material}))
      {
        return false;
      }
      if (std::ranges::any_of(faces, [&](const auto& face) {
            return isMaterialMissing(map, face.face());
          }))
      {
        context.outcome.warnings.emplace_back("missing_material");
      }
      return true;
    },
    "cannot set the material");
}

void checkSetMaterialArgs(const Json& args)
{
  if (args["material"].asString().empty())
  {
    throw invalidArgument("set_material: \"material\" must not be empty");
  }
}

void setUv(OpContext& context)
{
  auto update = mdl::UpdateBrushFaceAttributes{};
  if (const auto* offset = context.args.find("offset"))
  {
    std::tie(update.xOffset, update.yOffset) = valueOps(*offset);
  }
  if (const auto* scale = context.args.find("scale"))
  {
    std::tie(update.xScale, update.yScale) = valueOps(*scale);
  }
  if (const auto* rotation = context.args.find("rotation"))
  {
    update.rotation = valueOp(*rotation);
  }

  editFaces(
    context,
    [&](auto& map, const auto& faces) {
      // the map library accepts a scale of 0, which verify reports as invalid
      for (const auto& face : faces)
      {
        auto updated = face.face();
        if (
          mdl::evaluate(update, updated).is_success()
          && (updated.uvAttributes().scale.x() == 0 || updated.uvAttributes().scale.y() == 0))
        {
          throw faceError(context.opName, face, "would get a UV scale of 0");
        }
      }
      return mdl::setBrushFaceAttributes(map, faces, update);
    },
    "cannot set the UV");
}

void checkSetUvArgs(const Json& args)
{
  if (!args.find("offset") && !args.find("scale") && !args.find("rotation"))
  {
    throw invalidArgument("set_uv: give at least one of offset, scale and rotation");
  }

  const auto* scale = args.find("scale");
  if (scale && !scale->find("add"))
  {
    if (const auto [u, v] = pairOf(*scale); u == 0 || v == 0)
    {
      throw invalidArgument(
        fmt::format("set_uv: \"scale\" must not be 0, not {}", scale->dump()));
    }
  }
}

void fitUv(OpContext& context)
{
  editFaces(
    context,
    [&](auto& map, const auto& faces) {
      // without the material's size, the fit would scale the UV to a 1x1 texture
      for (const auto& face : faces)
      {
        if (face.face().material() == nullptr)
        {
          throw faceError(
            context.opName,
            face,
            fmt::format(
              "has the material '{}', which is not loaded, but fitting needs its size; "
              "set quake_dir in tbcli.json and list the material's wad in the "
              "worldspawn's wad property",
              face.face().materialName()));
        }
      }
      return mdl::autoFitUv(map, faces);
    },
    "cannot fit the UV");
}

void resetUv(OpContext& context)
{
  const auto& defaultUvAttributes =
    context.map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes;
  editFaces(
    context,
    [&](auto& map, const auto& faces) {
      return mdl::setBrushFaceAttributes(map, faces, mdl::resetAll(defaultUvAttributes));
    },
    "cannot reset the UV");
}

} // namespace tb::cli
