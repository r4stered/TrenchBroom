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


#pragma once

#include "cli/NodePath.h"

#include "vm/vec.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace tb::mdl
{
class BrushFaceHandle;
} // namespace tb::mdl

namespace tb::cli
{

class Json;
struct OpContext;

/** The directions a face selector can name: `+x`, `-x`, `+y`, `-y`, `+z` and `-z`. */
std::span<const std::string_view> faceDirectionNames();

/** Which faces of brush targets face ops act on. */
struct FaceSelector
{
  /** The directions whose faces to pick, or nothing to pick all faces. */
  std::optional<std::vector<vm::vec3d>> directions;
};

/**
 * The selector that the given JSON describes, if it is well-formed: `"all"`, one of the
 * directions `"+x"`, `"-x"`, `"+y"`, `"-y"`, `"+z"` and `"-z"`, or a non-empty list of
 * directions.
 */
std::optional<FaceSelector> parseFaceSelector(const Json& value);

/** Whether the given JSON is a well-formed face selector (see parseFaceSelector). */
bool isFaceSelector(const Json& value);

/**
 * The faces that face op targets address: each face path, and the faces of each brush
 * that the selector picks, in order and each once. A direction picks the faces whose
 * normals are within 0.5 degrees of it.
 *
 * @param targets brushes and faces of brushes
 */
std::vector<mdl::BrushFaceHandle> selectFaces(
  const std::vector<NodeOrFace>& targets, const FaceSelector& selector);

/*
 * The face ops: set_material, set_uv, fit_uv and reset_uv. Each acts on the faces that
 * selectFaces picks from its `target` and `faces`, and reports the brushes whose faces
 * changed as modified. Without `faces`, every face of the brush targets is picked;
 * `faces` together with a face path target is refused with `invalid_argument`. If the
 * targets name brushes but the selector picks none of their faces, the op warns
 * `no_faces_selected`.
 */

/**
 * `set_material {target, faces, material}`: sets the faces' material. If the map's
 * materials can be checked and the material is not among them, the op warns
 * `missing_material`.
 */
void setMaterial(OpContext& context);

/**
 * Checks the arguments of `set_material`.
 *
 * @throw CliError `invalid_argument` if the material is empty
 */
void checkSetMaterialArgs(const Json& args);

/**
 * `set_uv {target, faces, offset?, scale?, rotation?}`: sets the faces' UV offset, scale
 * and rotation to the given values, or adds `{"add": ...}` values to them. An offset or
 * scale is a pair `[u, v]` or one number for both.
 *
 * @throw CliError `op_failed` if a scale of a face would become 0
 */
void setUv(OpContext& context);

/**
 * Checks the arguments of `set_uv`.
 *
 * @throw CliError `invalid_argument` unless offset, scale or rotation is given, or if a
 * scale set to a value is 0
 */
void checkSetUvArgs(const Json& args);

/**
 * `fit_uv {target, faces}`: aligns, justifies and scales the UV of each face so that
 * its material covers it exactly once, as the editor's auto fit does.
 *
 * @throw CliError `op_failed` if a face's material is not loaded, since fitting needs
 * its size
 */
void fitUv(OpContext& context);

/**
 * `reset_uv {target, faces}`: resets the UV of the faces to the game's defaults: offset
 * and rotation 0, the default scale and the face's default axes.
 */
void resetUv(OpContext& context);

} // namespace tb::cli
