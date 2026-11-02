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

#include "cli/EditOps.h"

#include "base/Result.h"
#include "cli/CliError.h"
#include "cli/CliValidators.h"
#include "cli/GeometryJson.h"
#include "cli/IssueTypes.h"
#include "cli/Ops.h"
#include "cli/ParseUtils.h"
#include "cli/Targets.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CircleShape.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityDefinitionUtils.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/NodeContents.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/flat_map.h"
#include "kd/ranges/to.h"
#include "kd/result.h"
#include "kd/string_compare.h"
#include "kd/string_utils.h"
#include "kd/vector_utils.h"

#include "vm/bbox.h"
#include "vm/vec.h"
#include "vm/vec_io.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/**
 * Checks the classname and properties that a create op is given.
 *
 * @param reservedKeys the keys that the op sets from its own arguments
 */
void checkNewEntityArgs(
  const std::string_view opName,
  const Json& args,
  const std::initializer_list<std::string_view> reservedKeys)
{
  const auto& classname = args["classname"].asString();
  if (classname.empty())
  {
    throw invalidArgument(fmt::format("{}: \"classname\" must not be empty", opName));
  }
  if (classname == mdl::EntityPropertyValues::WorldspawnClassname)
  {
    throw invalidArgument(
      fmt::format(
        "{}: cannot create a worldspawn entity; the map's only one is at path {}",
        opName,
        WorldPath));
  }

  if (const auto* properties = args.find("properties"))
  {
    for (const auto& [key, value] : properties->asObject())
    {
      if (key.empty())
      {
        throw invalidArgument(
          fmt::format("{}: a property key must not be empty", opName));
      }
      if (std::ranges::find(reservedKeys, key) != reservedKeys.end())
      {
        throw invalidArgument(
          fmt::format(
            "{}: give \"{}\" as its own argument, not in \"properties\"", opName, key));
      }
    }
  }
}

/**
 * The definition of the classname that a create op is given. It must be of the given
 * type unless undefined classes are allowed; then it is the definition if there is one.
 */
const mdl::EntityDefinition* checkedDefinition(
  const OpContext& context, const mdl::EntityDefinitionType type)
{
  const auto opName = context.opName;
  const auto& classname = context.args["classname"].asString();
  const auto* definition = context.map.entityDefinitionManager().definition(classname);
  if (
    (definition && mdl::getType(*definition) == type)
    || context.args["allow_undefined"].asBool())
  {
    return definition;
  }

  const auto typeName = [](const auto definitionType) {
    return definitionType == mdl::EntityDefinitionType::Point ? "point" : "brush";
  };
  throw CliError{
    ErrorCode::UndefinedClass,
    definition ? fmt::format(
                   "{}: \"{}\" is a {} class, not a {} class; pass allow_undefined to "
                   "create it anyway",
                   opName,
                   classname,
                   typeName(mdl::getType(*definition)),
                   typeName(type))
               : fmt::format(
                   "{}: the FGD does not define \"{}\"; pass allow_undefined to create "
                   "it anyway",
                   opName,
                   classname)};
}

/**
 * A new entity with, in this order, the classname of the op's arguments, the
 * definition's default properties if the game config asks for them, the given origin
 * and the properties of the op's arguments.
 */
mdl::Entity newEntity(
  const OpContext& context,
  const mdl::EntityDefinition* definition,
  const std::optional<vm::vec3d>& origin)
{
  auto entity = mdl::Entity{
    {{mdl::EntityPropertyKeys::Classname, context.args["classname"].asString()}}};
  if (definition && context.map.worldNode().entityPropertyConfig().setDefaultProperties)
  {
    mdl::setDefaultProperties(*definition, entity, mdl::SetDefaultPropertyMode::SetAll);
  }
  if (origin)
  {
    entity.setOrigin(*origin);
  }
  if (const auto* properties = context.args.find("properties"))
  {
    for (const auto& [key, value] : properties->asObject())
    {
      entity.addOrUpdateProperty(key, value.asString());
    }
  }
  return entity;
}

/** The entity node of a target of a property op: the world or an entity. */
const mdl::EntityNodeBase& entityNodeOf(const mdl::Node& node)
{
  const auto* entityNode = dynamic_cast<const mdl::EntityNodeBase*>(&node);
  contract_assert(entityNode != nullptr);
  return *entityNode;
}

/**
 * Applies `edit` to a copy of the entity of each target and swaps in the copies that
 * changed, which are reported as modified. As in the editor, the changes reach the
 * other members of linked groups.
 *
 * The map library's property functions act on the selection, which cannot hold the
 * world together with other entities, so the copies are swapped in directly.
 */
void editEntities(
  OpContext& context,
  const std::function<void(const mdl::EntityNodeBase&, mdl::Entity&)>& edit)
{
  auto nodesToSwap = std::vector<std::pair<mdl::Node*, mdl::NodeContents>>{};
  auto changed = std::vector<mdl::Node*>{};
  for (const auto& [node, faceIndex] : context.targets("target"))
  {
    const auto& entityNode = entityNodeOf(*node);
    auto entity = entityNode.entity();
    edit(entityNode, entity);
    if (entity != entityNode.entity())
    {
      changed.push_back(node);
      nodesToSwap.emplace_back(node, mdl::NodeContents{std::move(entity)});
    }
  }
  if (changed.empty())
  {
    return;
  }

  if (!mdl::updateNodeContents(
        context.map,
        std::string{context.opName},
        std::move(nodesToSwap),
        mdl::collectContainingGroups(changed)))
  {
    throw CliError{
      ErrorCode::OpFailed, fmt::format("{}: cannot change the entities", context.opName)};
  }
  std::ranges::copy(changed, std::back_inserter(context.outcome.modified));
}

/**
 * Checks a key that a property op is given: it must not be empty, and it must not be
 * classname, which only the create ops set, checked against the FGD.
 */
void checkPropertyKey(
  const std::string_view opName, const std::string_view argName, const Json& args)
{
  const auto& key = args[argName].asString();
  if (key.empty())
  {
    throw invalidArgument(fmt::format("{}: \"{}\" must not be empty", opName, argName));
  }
  if (key == mdl::EntityPropertyKeys::Classname)
  {
    throw invalidArgument(
      fmt::format(
        "{}: cannot change classname, which the create ops check against the FGD; "
        "create a new entity instead (create_brush_entity can take another entity's "
        "brushes)",
        opName));
  }
}

/** The value of the spawnflag that `flag` gives for the given entity. */
int spawnflagValue(const Json& flag, const mdl::EntityNodeBase& entityNode)
{
  if (flag.isNumber())
  {
    return static_cast<int>(flag.asNumber());
  }

  const auto& name = flag.asString();
  const auto& entity = entityNode.entity();
  if (const auto* flags = mdl::getSpawnflagsDefinition(entity.definition()))
  {
    for (const auto& each : flags->flags)
    {
      if (kdl::ci::str_is_equal(each.shortDescription, name))
      {
        return each.value;
      }
    }
  }

  const auto path = nodePath(entityNode);
  throw CliError{
    ErrorCode::OpFailed,
    fmt::format(
      "set_spawnflag: {} ({}) has no spawnflag named \"{}\"; tbcli fgd {} lists them, "
      "or give the flag's value",
      path,
      entity.classname(),
      name,
      entity.classname()),
    path};
}

/** The validator that the map registers for the given issue type. */
const mdl::Validator& registeredValidatorOf(
  const mdl::WorldNode& worldNode, const IssueTypeDef& type)
{
  for (const auto* validator : worldNode.registeredValidators())
  {
    if (findIssueType(*validator) == &type)
    {
      return *validator;
    }
  }
  throw CliError{
    ErrorCode::InternalError,
    fmt::format("fix: the map has no validator for issue type \"{}\"", type.id)};
}

/** The validator's quick-fix that the given table entry describes. */
const mdl::IssueQuickFix& quickFixOf(
  const mdl::Validator& validator, const QuickFixDef& fix)
{
  for (const auto* quickFix : validator.quickFixes())
  {
    if (quickFix->description() == fix.description)
    {
      return *quickFix;
    }
  }
  throw CliError{
    ErrorCode::InternalError,
    fmt::format(
      "fix: the validator '{}' has no quick-fix '{}'",
      validator.description(),
      fix.description)};
}

/**
 * The issues that the validator finds on the given nodes and faces and the nodes inside
 * them, each once. Issues on locked nodes are left out unless `allowLocked` is set.
 */
std::vector<std::unique_ptr<mdl::Issue>> issuesOf(
  const mdl::Validator& validator,
  const std::vector<NodeOrFace>& scopes,
  const bool allowLocked)
{
  auto result = std::vector<std::unique_ptr<mdl::Issue>>{};
  const auto validate = [&](mdl::Node& node, const std::optional<size_t> faceIndex) {
    if (!allowLocked && node.locked())
    {
      return;
    }
    auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
    validator.validate(node, issues);
    for (auto& issue : issues)
    {
      const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(issue.get());
      if (!faceIndex || (faceIssue && faceIssue->faceIndex() == *faceIndex))
      {
        result.push_back(std::move(issue));
      }
    }
  };

  // whole nodes first, so that a face inside one of them is not fixed twice
  auto visited = std::unordered_set<const mdl::Node*>{};
  for (const auto& [node, faceIndex] : scopes)
  {
    if (!faceIndex)
    {
      for (auto* each : nodeAndDescendants(*node))
      {
        if (visited.insert(each).second)
        {
          validate(*each, std::nullopt);
        }
      }
    }
  }
  for (const auto& [node, faceIndex] : scopes)
  {
    if (faceIndex && !visited.contains(node))
    {
      validate(*node, faceIndex);
    }
  }
  return result;
}

/** What a quick-fix may change, recorded before it runs. */
struct FixScope
{
  /** The nodes with issues, each once. */
  std::vector<mdl::Node*> issueNodes;
  /** The children of the nodes with issues, with their parents. */
  std::vector<std::pair<mdl::Node*, mdl::Node*>> childParents;
  /** The ancestors of the nodes with issues, which a fix may leave empty. */
  std::vector<mdl::Node*> ancestors;
};

FixScope fixScopeOf(const std::vector<std::unique_ptr<mdl::Issue>>& issues)
{
  auto result = FixScope{};
  for (const auto& issue : issues)
  {
    appendOnce(result.issueNodes, &issue->node());
  }
  for (auto* node : result.issueNodes)
  {
    for (auto* child : node->children())
    {
      result.childParents.emplace_back(child, node);
    }
  }
  result.ancestors = ancestorsOf(result.issueNodes);
  return result;
}

/**
 * The nodes the issue browser selects for the given issues, on which quick-fixes act:
 * none if any issue is on a node that cannot be selected, such as the world.
 */
std::vector<mdl::Node*> selectionFor(
  const std::vector<std::unique_ptr<mdl::Issue>>& issues)
{
  auto result = std::vector<mdl::Node*>{};
  for (const auto& issue : issues)
  {
    if (!issue->addSelectableNodes(result))
    {
      return {};
    }
  }
  kdl::vec_sort_and_remove_duplicates(result);
  return result;
}

/**
 * Records what a quick-fix changed: the nodes with issues that are still in the map and
 * the children it moved as modified, and the nodes with issues and their ancestors that
 * are gone as removed.
 */
void recordFixOutcome(
  const FixScope& scope, const mdl::WorldNode& worldNode, OpOutcome& outcome)
{
  for (auto* node : scope.issueNodes)
  {
    appendOnce(isInMap(*node, worldNode) ? outcome.modified : outcome.removed, node);
  }
  for (const auto& [child, parent] : scope.childParents)
  {
    if (isInMap(*child, worldNode) && child->parent() != parent)
    {
      appendOnce(outcome.modified, child);
    }
  }
  appendRemovedNodes(scope.ancestors, worldNode, outcome.removed);
}

/** The builder that the editor's brush tools use, with the game's default UV. */
mdl::BrushBuilder brushBuilderOf(const mdl::Map& map)
{
  const auto& faceAttribsConfig = map.gameInfo().gameConfig.faceAttribsConfig;
  return mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    faceAttribsConfig.defaultUvAttributes,
    faceAttribsConfig.defaultSurfaceAttributes};
}

/** A built brush as a list of one, as addBrushes takes it. */
Result<std::vector<mdl::Brush>> asList(Result<mdl::Brush> brush)
{
  return std::move(brush)
         | kdl::transform([](auto b) { return std::vector{std::move(b)}; });
}

/**
 * Adds the brushes that a create op built to its `parent` and reports them as created.
 * Warns `missing_material` if the map's materials can be checked and a face's material
 * is not among them.
 *
 * @throw CliError `op_failed` if the brushes could not be built or added
 */
void addBrushes(OpContext& context, Result<std::vector<mdl::Brush>> brushes)
{
  const auto opName = context.opName;
  if (!brushes)
  {
    throw CliError{
      ErrorCode::OpFailed,
      fmt::format(
        "{}: cannot create the brush: {}", opName, std::get<Error>(brushes.error()).msg)};
  }

  auto& map = context.map;
  auto brushNodes = std::vector<std::unique_ptr<mdl::BrushNode>>{};
  auto nodes = std::vector<mdl::Node*>{};
  for (auto& brush : brushes.value())
  {
    brushNodes.push_back(std::make_unique<mdl::BrushNode>(std::move(brush)));
    nodes.push_back(brushNodes.back().get());
  }
  if (mdl::addNodes(map, {{context.node("parent").node, nodes}}).empty())
  {
    throw CliError{
      ErrorCode::OpFailed, fmt::format("{}: cannot add the brush to the map", opName)};
  }

  auto missingMaterial = false;
  for (auto& brushNode : brushNodes)
  {
    missingMaterial = missingMaterial
                      || std::ranges::any_of(
                        brushNode->brush().faces(),
                        [&](const auto& face) { return isMaterialMissing(map, face); });
    context.outcome.created.push_back(brushNode.release());
  }
  if (missingMaterial)
  {
    context.outcome.warnings.emplace_back("missing_material");
  }
}

/**
 * Checks the `material` argument of a create op.
 *
 * @throw CliError `invalid_argument` if it is empty
 */
void checkMaterialArg(const std::string_view opName, const Json& args)
{
  if (args["material"].asString().empty())
  {
    throw invalidArgument(fmt::format("{}: \"material\" must not be empty", opName));
  }
}

/** The names of the shape kinds, in the order of ShapeKind. */
constexpr auto ShapeKindNames =
  std::array<std::string_view, 3>{"cylinder", "cone", "arch"};

/** The range of a shape's sides, as in the editor's shape tool. */
constexpr auto MinShapeSides = 3;
constexpr auto MaxShapeSides = 96;

/** The thickness of an arch's band unless one is given, as in the editor's shape tool. */
constexpr auto DefaultArchThickness = 16.0;

/** The brushes of the shape that create_shape's arguments describe. */
Result<std::vector<mdl::Brush>> buildShape(const mdl::Map& map, const Json& args)
{
  const auto bounds = bboxFromJson(args["bbox"]);
  const auto circleShape = mdl::CircleShape{
    mdl::EdgeAlignedCircle{static_cast<size_t>(args["sides"].asNumber())}};
  const auto axis = *parseAxis(args["axis"].asString());
  const auto& material = args["material"].asString();
  const auto builder = brushBuilderOf(map);

  switch (*parseShapeKind(args["kind"].asString()))
  {
  case ShapeKind::Cylinder:
    return asList(builder.createCylinder(bounds, circleShape, axis, material));
  case ShapeKind::Cone:
    return asList(builder.createCone(bounds, circleShape, axis, material));
  default: // ShapeKind::Arch
    return builder.createArch(
      bounds,
      args.find("thickness") ? args["thickness"].asNumber() : DefaultArchThickness,
      circleShape,
      axis,
      material);
  }
}

} // namespace

void createBox(OpContext& context)
{
  const auto& args = context.args;
  const auto min = vec3FromJson(args["min"]);
  const auto max = vec3FromJson(args["max"]);
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(min[i] < max[i]))
    {
      throw invalidArgument(
        fmt::format(
          "create_box: min {} must be less than max {} on every axis",
          args["min"].dump(),
          args["max"].dump()));
    }
  }

  checkMaterialArg("create_box", args);

  addBrushes(
    context,
    asList(brushBuilderOf(context.map)
             .createCuboid(vm::bbox3d{min, max}, args["material"].asString())));
}

void createBrush(OpContext& context)
{
  const auto points = context.args["vertices"].asArray()
                      | std::views::transform(vec3FromJson)
                      | kdl::ranges::to<std::vector>();
  addBrushes(
    context,
    asList(brushBuilderOf(context.map)
             .createBrush(points, context.args["material"].asString())));
}

void checkCreateBrushArgs(const Json& args)
{
  checkMaterialArg("create_brush", args);
}

std::span<const std::string_view> shapeKindNames()
{
  return ShapeKindNames;
}

std::optional<ShapeKind> parseShapeKind(const std::string_view name)
{
  const auto it = std::ranges::find(ShapeKindNames, name);
  return it != ShapeKindNames.end()
           ? std::optional{static_cast<ShapeKind>(it - ShapeKindNames.begin())}
           : std::nullopt;
}

void createShape(OpContext& context)
{
  auto brushes = buildShape(context.map, context.args);
  if (brushes && brushes.value().empty())
  {
    // the editor's arch skips the pieces that the bbox leaves no room for
    throw CliError{
      ErrorCode::OpFailed, "create_shape: the bbox is too small for the shape"};
  }
  addBrushes(context, std::move(brushes));
}

void checkCreateShapeArgs(const Json& args)
{
  const auto bounds = bboxFromJson(args["bbox"]);
  for (size_t i = 0; i < 3; ++i)
  {
    if (!(bounds.min[i] < bounds.max[i]))
    {
      throw invalidArgument(
        fmt::format(
          "create_shape: the bbox's min must be less than its max on every axis, not {}",
          args["bbox"].dump()));
    }
  }

  const auto sides = args["sides"].asNumber();
  if (std::trunc(sides) != sides || sides < MinShapeSides || sides > MaxShapeSides)
  {
    throw invalidArgument(
      fmt::format(
        "create_shape: \"sides\" must be a whole number from {} to {}, not {}",
        MinShapeSides,
        MaxShapeSides,
        args["sides"].dump()));
  }

  if (args.find("thickness"))
  {
    if (*parseShapeKind(args["kind"].asString()) != ShapeKind::Arch)
    {
      throw invalidArgument(
        "create_shape: \"thickness\" is only for arches, the thickness of their band");
    }
    checkPositiveArg("create_shape", "thickness", args);
  }

  checkMaterialArg("create_shape", args);
}

void createPointEntity(OpContext& context)
{
  const auto* definition = checkedDefinition(context, mdl::EntityDefinitionType::Point);
  const auto& parent = context.node("parent");

  auto entityNode = std::make_unique<mdl::EntityNode>(
    newEntity(context, definition, vec3FromJson(context.args["origin"])));
  if (mdl::addNodes(context.map, {{parent.node, {entityNode.get()}}}).empty())
  {
    throw CliError{
      ErrorCode::OpFailed, "create_point_entity: cannot add the entity to the map"};
  }
  context.outcome.created.push_back(entityNode.release());
}

void checkCreatePointEntityArgs(const Json& args)
{
  checkNewEntityArgs(
    "create_point_entity",
    args,
    {mdl::EntityPropertyKeys::Classname, mdl::EntityPropertyKeys::Origin});
}

void createBrushEntity(OpContext& context)
{
  auto brushes = std::vector<mdl::Node*>{};
  for (const auto& [node, faceIndex] : context.targets("brushes"))
  {
    brushes.push_back(node);
  }
  if (brushes.empty())
  {
    throw CliError{
      ErrorCode::WrongNodeKind,
      "create_brush_entity: brushes names no brushes, but a brush entity needs one"};
  }

  const auto* definition = checkedDefinition(context, mdl::EntityDefinitionType::Brush);

  auto& map = context.map;
  const auto ancestors = ancestorsOf(brushes);
  auto& parent = mdl::parentForNodes(map, brushes);

  auto entityNode =
    std::make_unique<mdl::EntityNode>(newEntity(context, definition, std::nullopt));
  if (mdl::addNodes(map, {{&parent, {entityNode.get()}}}).empty())
  {
    throw CliError{
      ErrorCode::OpFailed, "create_brush_entity: cannot add the entity to the map"};
  }
  auto* entity = entityNode.release();
  if (!mdl::reparentNodes(map, {{entity, brushes}}))
  {
    throw CliError{
      ErrorCode::OpFailed,
      "create_brush_entity: cannot move the brushes into the entity"};
  }

  context.outcome.created.push_back(entity);
  recordMoveOutcome(brushes, ancestors, map.worldNode(), context.outcome);
}

void checkCreateBrushEntityArgs(const Json& args)
{
  checkNewEntityArgs("create_brush_entity", args, {mdl::EntityPropertyKeys::Classname});
}

void moveToWorld(OpContext& context)
{
  auto& map = context.map;

  // the brushes of an entity go to the group or layer that holds the entity
  auto toMove = kdl::flat_map<mdl::Node*, std::vector<mdl::Node*>>{};
  auto moved = std::vector<mdl::Node*>{};
  auto visited = std::unordered_set<const mdl::Node*>{};
  const auto move = [&](mdl::Node& entity, mdl::Node* brush) {
    if (visited.insert(brush).second)
    {
      toMove[&mdl::parentForNodes(map, {&entity})].push_back(brush);
      moved.push_back(brush);
    }
  };

  for (const auto& [node, faceIndex] : context.targets("target"))
  {
    if (kindOf(*node) == NodeKind::Entity)
    {
      for (auto* child : node->children())
      {
        move(*node, child);
      }
    }
    else if (auto* parent = node->parent(); kindOf(*parent) == NodeKind::Entity)
    {
      move(*parent, node);
    }
  }
  if (moved.empty())
  {
    return;
  }

  const auto ancestors = ancestorsOf(moved);
  if (!mdl::reparentNodes(map, toMove))
  {
    throw CliError{ErrorCode::OpFailed, "move_to_world: cannot move the brushes"};
  }

  recordMoveOutcome(moved, ancestors, map.worldNode(), context.outcome);
}

void setProperty(OpContext& context)
{
  const auto& key = context.args["key"].asString();
  const auto& value = context.args["value"].asString();
  editEntities(
    context, [&](const auto&, auto& entity) { entity.addOrUpdateProperty(key, value); });
}

void checkSetPropertyArgs(const Json& args)
{
  checkPropertyKey("set_property", "key", args);

  const auto& key = args["key"].asString();
  const auto& value = args["value"].asString();
  if (key == mdl::EntityPropertyKeys::Origin && !vm::parse<double, 3>(value))
  {
    throw invalidArgument(
      fmt::format(
        "set_property: origin must be three numbers such as \"0 0 64\", not \"{}\"",
        value));
  }
  if (key == mdl::EntityPropertyKeys::Angle && !parseDecimal(value))
  {
    throw invalidArgument(
      fmt::format(
        "set_property: angle must be a number such as \"90\", not \"{}\"", value));
  }
}

void removeProperty(OpContext& context)
{
  const auto& key = context.args["key"].asString();
  auto missing = false;
  editEntities(context, [&](const auto&, auto& entity) {
    missing = missing || !entity.hasProperty(key);
    entity.removeProperty(key);
  });
  if (missing)
  {
    context.outcome.warnings.emplace_back("property_not_found");
  }
}

void checkRemovePropertyArgs(const Json& args)
{
  checkPropertyKey("remove_property", "key", args);
}

void renameProperty(OpContext& context)
{
  const auto& from = context.args["from"].asString();
  const auto& to = context.args["to"].asString();
  auto missing = false;
  editEntities(context, [&](const auto& entityNode, auto& entity) {
    if (!entity.hasProperty(from))
    {
      missing = true;
      return;
    }
    if (entity.hasProperty(to))
    {
      const auto path = nodePath(entityNode);
      throw CliError{
        ErrorCode::OpFailed,
        fmt::format(
          "rename_property: {} has a property \"{}\" already; remove it first", path, to),
        path};
    }
    entity.renameProperty(from, to);
  });
  if (missing)
  {
    context.outcome.warnings.emplace_back("property_not_found");
  }
}

void checkRenamePropertyArgs(const Json& args)
{
  checkPropertyKey("rename_property", "from", args);
  checkPropertyKey("rename_property", "to", args);
  if (args["from"] == args["to"])
  {
    throw invalidArgument("rename_property: \"from\" and \"to\" must differ");
  }
}

void setSpawnflag(OpContext& context)
{
  const auto& flag = context.args["flag"];
  const auto on = context.args["on"].asBool();
  editEntities(context, [&](const auto& entityNode, auto& entity) {
    const auto flagValue = spawnflagValue(flag, entityNode);
    const auto* oldValue = entity.property(mdl::EntityPropertyKeys::Spawnflags);
    // as in the editor, a malformed value counts as no flags
    const auto oldFlags = oldValue ? kdl::str_to_int(*oldValue).value_or(0) : 0;
    const auto newFlags = on ? oldFlags | flagValue : oldFlags & ~flagValue;
    if (newFlags != oldFlags)
    {
      entity.addOrUpdateProperty(
        mdl::EntityPropertyKeys::Spawnflags, kdl::str_to_string(newFlags));
    }
  });
}

void deleteNode(OpContext& context)
{
  // a node inside another target is removed with it
  auto targets = std::vector<mdl::Node*>{};
  for (const auto& [node, faceIndex] : context.targets("target"))
  {
    targets.push_back(node);
  }
  if (targets.empty())
  {
    return;
  }

  auto toRemove = targets;
  std::erase_if(
    toRemove, [&](const auto* node) { return node->isDescendantOf(targets); });

  const auto ancestors = ancestorsOf(toRemove);

  auto& worldNode = context.map.worldNode();
  mdl::removeNodes(context.map, toRemove);

  // Removing the last child of an entity or group also removes the emptied parent.
  auto& removed = context.outcome.removed;
  removed = targets;
  appendRemovedNodes(ancestors, worldNode, removed);
}

void checkFixArgs(const Json& args)
{
  const auto& typeId = args["issue_type"].asString();
  const auto* type = findIssueType(typeId);
  if (!type)
  {
    throw invalidArgument(
      fmt::format(
        "fix: unknown issue type \"{}\"; tbcli ops --json lists the issue types",
        typeId));
  }

  const auto& fixId = args["fix"].asString();
  if (!findQuickFix(*type, fixId))
  {
    const auto fixIds = type->fixes | std::views::transform([](const auto& fix) {
                          return fmt::format("\"{}\"", fix.id);
                        })
                        | kdl::ranges::to<std::vector>();
    throw invalidArgument(
      fixIds.empty() ? fmt::format("fix: issue type \"{}\" has no quick-fixes", typeId)
                     : fmt::format(
                         "fix: unknown fix \"{}\" for issue type \"{}\", which has {}",
                         fixId,
                         typeId,
                         kdl::str_join(fixIds, ", ")));
  }
}

void fixIssues(OpContext& context)
{
  const auto& type = *findIssueType(context.args["issue_type"].asString());
  const auto& fixDef = *findQuickFix(type, context.args["fix"].asString());

  auto& map = context.map;
  auto& worldNode = map.worldNode();
  const auto& validator = registeredValidatorOf(worldNode, type);
  const auto& quickFix = quickFixOf(validator, fixDef);

  const auto scopes = context.args.find("target")
                        ? context.targets("target")
                        : std::vector<NodeOrFace>{{&worldNode, std::nullopt}};
  const auto issues = issuesOf(validator, scopes, context.allowLocked);
  if (issues.empty())
  {
    context.outcome.warnings.emplace_back("nothing_to_fix");
    return;
  }

  const auto scope = fixScopeOf(issues);
  mdl::deselectAll(map);
  mdl::selectNodes(map, selectionFor(issues));
  quickFix.apply(map, issues | std::views::transform([](const auto& issue) {
                        return static_cast<const mdl::Issue*>(issue.get());
                      }) | kdl::ranges::to<std::vector>());
  mdl::deselectAll(map);

  recordFixOutcome(scope, worldNode, context.outcome);
}

} // namespace tb::cli
