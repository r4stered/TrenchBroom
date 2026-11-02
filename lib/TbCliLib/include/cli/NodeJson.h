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

#include <cstddef>

namespace tb::mdl
{
class Brush;
class BrushNode;
class GroupNode;
class LayerNode;
class Node;
class PatchNode;
} // namespace tb::mdl

namespace tb::cli
{

class Json;
struct Summary;

/** `{brushes, entities, faces}` */
Json countsToJson(const Summary& summary);

/** `{min, max}`, or null if the summary has no bounds. */
Json boundsToJson(const Summary& summary);

/**
 * A layer: `{path, id, name, bounds, counts, hidden, locked, omit_from_export}`. `id` is
 * the persistent id or null, `hidden` and `locked` are the effective states, which a
 * node inherits from its parent unless it sets its own.
 */
Json layerToJson(const mdl::LayerNode& layerNode);

/** A group: `{path, id, name, bounds, counts, hidden, locked}`, as for layers. */
Json groupToJson(const mdl::GroupNode& groupNode);

/** Whether the brush is exactly six axis-aligned faces: a box along the axes. */
bool isAxialBox(const mdl::Brush& brush);

/**
 * A brush summary: `{path, aabb, axial_box, textures, entity}`. `axial_box` tells
 * whether the brush is exactly six axis-aligned faces, `textures` are the distinct
 * texture names in alphabetical order, and `entity` is the path of the entity the brush
 * belongs to (`world` for worldspawn brushes).
 */
Json brushSummaryToJson(const mdl::BrushNode& brushNode);

/**
 * A brush in full: its summary, its `vertices`, and `faces` in file order, each as
 * faceToJson writes it.
 */
Json brushToJson(const mdl::BrushNode& brushNode, bool raw);

/** A face summary: `{path, normal, texture}`. */
Json faceSummaryToJson(const mdl::BrushNode& brushNode, size_t faceIndex);

/**
 * A face in full: `{path, normal, texture, uv, vertices}`, where `uv` holds the Valve 220
 * UV parameters `{u_axis, v_axis, offset, scale, rotation}`. With `raw`, `points` adds
 * the three plane points as written in the file.
 */
Json faceToJson(const mdl::BrushNode& brushNode, size_t faceIndex, bool raw);

/** A patch summary: `{path, aabb, texture}`. */
Json patchSummaryToJson(const mdl::PatchNode& patchNode);

/**
 * The summary of any node: the world and entities as boundedEntityToJson writes them,
 * layers and groups as layerToJson and groupToJson, brushes and patches as their
 * summaries.
 */
Json nodeSummaryToJson(const mdl::Node& node);

} // namespace tb::cli
