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

#include "cli/Ops.h"

#include "cli/CliError.h"
#include "cli/EditOps.h"
#include "cli/FaceOps.h"
#include "cli/IssueTypes.h"
#include "cli/ParseUtils.h"
#include "cli/TransformOps.h"

#include "kd/contracts.h"
#include "kd/string_utils.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <set>
#include <string_view>
#include <vector>

namespace tb::cli
{
namespace
{

bool isString(const Json& value)
{
  return value.isString();
}

bool isVec3(const Json& value)
{
  return value.isArray() && value.size() == 3
         && std::ranges::all_of(value.asArray(), &Json::isNumber);
}

bool isBinding(const Json& value)
{
  return value.isString() && isBindingName(value.asString());
}

bool isBool(const Json& value)
{
  return value.isBool();
}

bool isProperties(const Json& value)
{
  return value.isObject()
         && std::ranges::all_of(
           value.asObject(), [](const auto& member) { return member.value.isString(); });
}

/** The value of the highest spawnflag bit that the map writer and decoder handle. */
constexpr auto MaxSpawnFlagValue = 1 << 30;

bool isSpawnFlag(const Json& value)
{
  if (value.isString())
  {
    return !value.asString().empty();
  }
  if (!value.isNumber())
  {
    return false;
  }
  const auto number = value.asNumber();
  if (number < 1 || number > MaxSpawnFlagValue || std::trunc(number) != number)
  {
    return false;
  }
  const auto flag = static_cast<int>(number);
  return (flag & (flag - 1)) == 0;
}

bool isNumber(const Json& value)
{
  return value.isNumber();
}

bool isAxis(const Json& value)
{
  return value.isString() && parseAxis(value.asString()).has_value();
}

bool isBox(const Json& value)
{
  const auto* min = value.find("min");
  const auto* max = value.find("max");
  return value.isObject() && value.size() == 2 && min && max && isVec3(*min)
         && isVec3(*max);
}

/** A number, or a pair of numbers `[u, v]`. */
bool isNumberOrPair(const Json& value)
{
  return isNumber(value)
         || (value.isArray() && value.size() == 2 && std::ranges::all_of(value.asArray(), isNumber));
}

/** Whether the value is `{"add": <value>}` with a value that `accepts` accepts. */
bool isAddOf(const Json& value, bool (*accepts)(const Json&))
{
  const auto* add = value.find("add");
  return value.isObject() && value.size() == 1 && add && accepts(*add);
}

bool isNumberOrAdd(const Json& value)
{
  return isNumber(value) || isAddOf(value, isNumber);
}

bool isPairOrAdd(const Json& value)
{
  return isNumberOrPair(value) || isAddOf(value, isNumberOrPair);
}

Json stringSchema()
{
  auto result = Json::object();
  result.set("type", "string");
  return result;
}

Json vec3Schema()
{
  auto items = Json::object();
  items.set("type", "number");

  auto result = Json::object();
  result.set("type", "array");
  result.set("items", std::move(items));
  result.set("minItems", 3);
  result.set("maxItems", 3);
  return result;
}

Json boolSchema()
{
  auto result = Json::object();
  result.set("type", "boolean");
  return result;
}

Json propertiesSchema()
{
  auto result = Json::object();
  result.set("type", "object");
  result.set("additionalProperties", stringSchema());
  return result;
}

Json spawnFlagSchema()
{
  auto name = stringSchema();
  name.set("minLength", 1);

  auto value = Json::object();
  value.set("type", "integer");
  value.set("minimum", 1);
  value.set("maximum", MaxSpawnFlagValue);

  auto options = Json::array();
  options.push(std::move(name));
  options.push(std::move(value));

  auto result = Json::object();
  result.set("oneOf", std::move(options));
  return result;
}

Json numberSchema()
{
  auto result = Json::object();
  result.set("type", "number");
  return result;
}

/** A schema of one of the given strings. */
Json enumSchema(const auto& values)
{
  auto options = Json::array();
  for (const auto& value : values)
  {
    options.push(value);
  }

  auto result = Json::object();
  result.set("enum", std::move(options));
  return result;
}

Json axisSchema()
{
  return enumSchema(axisNames());
}

Json boxSchema()
{
  auto properties = Json::object();
  properties.set("min", vec3Schema());
  properties.set("max", vec3Schema());

  auto required = Json::array();
  required.push("min");
  required.push("max");

  auto result = Json::object();
  result.set("type", "object");
  result.set("properties", std::move(properties));
  result.set("required", std::move(required));
  result.set("additionalProperties", false);
  return result;
}

/** A number, or a pair of numbers. */
Json numberOrPairSchema()
{
  auto pair = Json::object();
  pair.set("type", "array");
  pair.set("items", numberSchema());
  pair.set("minItems", 2);
  pair.set("maxItems", 2);

  auto options = Json::array();
  options.push(numberSchema());
  options.push(std::move(pair));

  auto result = Json::object();
  result.set("oneOf", std::move(options));
  return result;
}

/** A schema of a value that `valueSchema` describes, or of `{"add": <value>}`. */
Json orAddSchema(Json valueSchema)
{
  auto properties = Json::object();
  properties.set("add", valueSchema);

  auto required = Json::array();
  required.push("add");

  auto add = Json::object();
  add.set("type", "object");
  add.set("properties", std::move(properties));
  add.set("required", std::move(required));
  add.set("additionalProperties", false);

  auto options = Json::array();
  options.push(std::move(valueSchema));
  options.push(std::move(add));

  auto result = Json::object();
  result.set("oneOf", std::move(options));
  return result;
}

Json numberOrAddSchema()
{
  return orAddSchema(numberSchema());
}

Json pairOrAddSchema()
{
  return orAddSchema(numberOrPairSchema());
}

Json faceSelectorSchema()
{
  auto list = Json::object();
  list.set("type", "array");
  list.set("items", enumSchema(faceDirectionNames()));
  list.set("minItems", 1);

  auto names = std::vector<std::string_view>{"all"};
  std::ranges::copy(faceDirectionNames(), std::back_inserter(names));

  auto options = Json::array();
  options.push(enumSchema(names));
  options.push(std::move(list));

  auto result = Json::object();
  result.set("oneOf", std::move(options));
  return result;
}

Json bindingSchema()
{
  auto result = stringSchema();
  result.set("pattern", "^\\$[A-Za-z_][A-Za-z0-9_]*$");
  return result;
}

/** A node path or a binding. */
Json nodePathSchema()
{
  auto result = stringSchema();
  result.set("pattern", "^([^$]|\\$[A-Za-z_][A-Za-z0-9_]*$)");
  return result;
}

Json targetSchema()
{
  auto list = Json::object();
  list.set("type", "array");
  list.set("items", nodePathSchema());
  list.set("minItems", 1);

  auto count = Json::object();
  count.set("type", "integer");
  count.set("minimum", 0);
  auto any = Json::object();
  any.set("const", "any");
  auto expectOptions = Json::array();
  expectOptions.push(std::move(count));
  expectOptions.push(std::move(any));
  auto expect = Json::object();
  expect.set("oneOf", std::move(expectOptions));
  expect.set("default", "any");

  auto queryProperties = Json::object();
  queryProperties.set("query", stringSchema());
  queryProperties.set("expect", std::move(expect));
  auto queryRequired = Json::array();
  queryRequired.push("query");
  auto query = Json::object();
  query.set("type", "object");
  query.set("properties", std::move(queryProperties));
  query.set("required", std::move(queryRequired));
  query.set("additionalProperties", false);

  auto options = Json::array();
  options.push(nodePathSchema());
  options.push(std::move(list));
  options.push(std::move(query));

  auto result = Json::object();
  result.set("oneOf", std::move(options));
  return result;
}

/** A flag value written as `count` numbers, as a JSON array. */
std::optional<Json> numbersFromFlag(const std::string& value, const size_t count)
{
  const auto numbers = parseNumberList(value);
  if (!numbers || numbers->size() != count)
  {
    return std::nullopt;
  }

  auto result = Json::array();
  for (const auto number : *numbers)
  {
    result.push(number);
  }
  return result;
}

std::optional<Json> vec3FromFlag(const std::string& value)
{
  return numbersFromFlag(value, 3);
}

std::optional<Json> stringFromFlag(const std::string& value)
{
  return Json{value};
}

std::optional<Json> boolFromFlag(const std::string& value)
{
  const auto result = parseBool(value);
  return result ? std::optional{Json{*result}} : std::nullopt;
}

std::optional<Json> propertiesFromFlag(const std::string& value)
{
  auto json = parseJson(value);
  return json ? std::optional{std::move(json).value()} : std::nullopt;
}

/** A spawnflag's value if the flag is a number, otherwise its name. */
std::optional<Json> spawnFlagFromFlag(const std::string& value)
{
  if (const auto number = parseUnsigned(value))
  {
    return Json{*number};
  }
  return Json{value};
}

std::optional<Json> numberFromFlag(const std::string& value)
{
  const auto number = parseDecimal(value);
  return number ? std::optional{Json{*number}} : std::nullopt;
}

/**
 * A flag value written as a JSON object if it starts with `{`, otherwise what `fromFlag`
 * makes of it.
 */
std::optional<Json> objectOrFromFlag(
  const std::string& value, std::optional<Json> (*fromFlag)(const std::string&))
{
  if (!value.starts_with('{'))
  {
    return fromFlag(value);
  }
  auto json = parseJson(value);
  return json && json.value().isObject() ? std::optional{std::move(json).value()}
                                         : std::nullopt;
}

/** A box written as six numbers `x1,y1,z1,x2,y2,z2`. */
std::optional<Json> boxFromNumbersFlag(const std::string& value)
{
  const auto numbers = numbersFromFlag(value, 6);
  if (!numbers)
  {
    return std::nullopt;
  }

  auto min = Json::array();
  auto max = Json::array();
  for (size_t i = 0; i < 3; ++i)
  {
    min.push((*numbers)[i]);
    max.push((*numbers)[i + 3]);
  }

  auto result = Json::object();
  result.set("min", std::move(min));
  result.set("max", std::move(max));
  return result;
}

/** A box written as JSON, or as six numbers `x1,y1,z1,x2,y2,z2`. */
std::optional<Json> boxFromFlag(const std::string& value)
{
  return objectOrFromFlag(value, boxFromNumbersFlag);
}

/** A number, or `{"add": n}` written as JSON. */
std::optional<Json> numberOrAddFromFlag(const std::string& value)
{
  return objectOrFromFlag(value, numberFromFlag);
}

/** A number or a pair `u,v`. */
std::optional<Json> pairFromFlag(const std::string& value)
{
  auto number = numberFromFlag(value);
  return number ? number : numbersFromFlag(value, 2);
}

/** A number, a pair `u,v`, or `{"add": n | [u, v]}` written as JSON. */
std::optional<Json> pairOrAddFromFlag(const std::string& value)
{
  return objectOrFromFlag(value, pairFromFlag);
}

/** A face selector written as `all`, a direction, or directions separated by commas. */
std::optional<Json> faceSelectorFromFlag(const std::string& value)
{
  const auto elements = kdl::str_split(value, ",");
  if (elements.size() == 1)
  {
    return Json{elements.front()};
  }

  auto result = Json::array();
  for (const auto& element : elements)
  {
    result.push(element);
  }
  return result;
}

/** How the arguments of one type are written, checked and described. */
struct ArgTypeTraits
{
  ArgType type;
  /** The value in help text. */
  std::string_view placeholder;
  /** What a value must be, for error messages. */
  std::string_view expected;
  bool (*accepts)(const Json& value);
  Json (*schema)();
  /** The JSON value of a subcommand flag's value, or nothing if it is malformed. */
  std::optional<Json> (*fromFlag)(const std::string& value);
};

const auto ArgTypes = std::array<ArgTypeTraits, 14>{{
  {ArgType::Vec3,
   "<x,y,z>",
   "three numbers [x, y, z] (x,y,z as a flag)",
   isVec3,
   vec3Schema,
   vec3FromFlag},
  {ArgType::String, "<string>", "a string", isString, stringSchema, stringFromFlag},
  {ArgType::NodePath,
   "<path>",
   "a node path or a binding such as \"$room\"",
   isPathOrBinding,
   nodePathSchema,
   stringFromFlag},
  {ArgType::Target,
   "<path>",
   "a node path, a binding, a non-empty list of them, or "
   "{\"query\": <ql>, \"expect\": <n> | \"any\"}",
   isTarget,
   targetSchema,
   stringFromFlag},
  {ArgType::Binding,
   "<$name>",
   "a binding name such as \"$room\"",
   isBinding,
   bindingSchema,
   stringFromFlag},
  {ArgType::Bool, "[true|false]", "true or false", isBool, boolSchema, boolFromFlag},
  {ArgType::Properties,
   "<json>",
   "an object of string values such as {\"key\": \"value\"}",
   isProperties,
   propertiesSchema,
   propertiesFromFlag},
  {ArgType::SpawnFlag,
   "<name | value>",
   "a spawnflag's name, or its value as a power of two from 1 to 1073741824",
   isSpawnFlag,
   spawnFlagSchema,
   spawnFlagFromFlag},
  {ArgType::Number, "<number>", "a number", isNumber, numberSchema, numberFromFlag},
  {ArgType::Axis, "<x|y|z>", "\"x\", \"y\" or \"z\"", isAxis, axisSchema, stringFromFlag},
  {ArgType::Box,
   "<x1,y1,z1,x2,y2,z2>",
   "a box {\"min\": [x, y, z], \"max\": [x, y, z]} (x1,y1,z1,x2,y2,z2 as a flag)",
   isBox,
   boxSchema,
   boxFromFlag},
  {ArgType::NumberOrAdd,
   "<n | {\"add\": n}>",
   "a number, or {\"add\": n} to add n",
   isNumberOrAdd,
   numberOrAddSchema,
   numberOrAddFromFlag},
  {ArgType::PairOrAdd,
   "<u,v | n | {\"add\": ...}>",
   "a pair [u, v] (u,v as a flag) or one number n for both, or {\"add\": [u, v] | n} "
   "to add it",
   isPairOrAdd,
   pairOrAddSchema,
   pairOrAddFromFlag},
  {ArgType::FaceSelector,
   "<all | +x,-z,...>",
   "\"all\", one of the directions \"+x\", \"-x\", \"+y\", \"-y\", \"+z\" and \"-z\", "
   "or a non-empty list of them (separated by commas as a flag)",
   isFaceSelector,
   faceSelectorSchema,
   faceSelectorFromFlag},
}};

const ArgTypeTraits& traits(const ArgType type)
{
  return *std::ranges::find(ArgTypes, type, &ArgTypeTraits::type);
}

/** The flag of an argument: its name with dashes for underscores. */
std::string flagName(const std::string_view argName)
{
  auto result = std::string{argName};
  std::ranges::replace(result, '_', '-');
  return result;
}

/**
 * Validates the arguments of an op given as an object without `op`.
 *
 * @throw CliError `invalid_argument` if an argument is unknown, malformed or missing
 */
Json checkArgs(const OpDef& def, const Json& given)
{
  for (const auto& member : given.asObject())
  {
    if (std::ranges::find(def.args, member.key, &ArgDef::name) == def.args.end())
    {
      throw invalidArgument(
        fmt::format("{}: unknown argument \"{}\"", def.name, member.key));
    }
  }

  // arguments are written in definition order, so that results are stable
  auto result = Json::object();
  for (const auto& arg : def.args)
  {
    if (const auto* value = given.find(arg.name))
    {
      const auto& argTraits = traits(arg.type);
      if (!argTraits.accepts(*value))
      {
        throw invalidArgument(
          fmt::format(
            "{}: \"{}\" must be {}, not {}",
            def.name,
            arg.name,
            argTraits.expected,
            value->dump()));
      }
      result.set(arg.name, *value);
    }
    else if (arg.required)
    {
      throw invalidArgument(
        fmt::format("{}: missing required argument \"{}\"", def.name, arg.name));
    }
    else if (arg.defaultValue)
    {
      result.set(arg.name, *arg.defaultValue);
    }
  }
  if (def.checkArgs)
  {
    def.checkArgs(result);
  }
  return result;
}

Json argSchema(const ArgDef& arg)
{
  auto result = traits(arg.type).schema();
  result.set("description", arg.description);
  if (arg.defaultValue)
  {
    result.set("default", *arg.defaultValue);
  }
  if (addressesNodes(arg))
  {
    auto kinds = Json::array();
    for (const auto kind : arg.rules.kinds)
    {
      kinds.push(toString(kind));
    }
    result.set("x-node-kinds", std::move(kinds));
    result.set("x-expands-to-brushes", arg.rules.expandsToBrushes);
  }
  return result;
}

/** The kinds an argument accepts, for help text. */
std::string kindsHelp(const ArgDef& arg)
{
  const auto kinds = toString(arg.rules.kinds);
  return arg.rules.expandsToBrushes
           ? kinds + "; layers, groups and brush entities stand for their brushes"
           : kinds;
}

/** The name of the Target argument that --query and --expect give. */
constexpr auto QueryTargetArg = std::string_view{"target"};

/** Whether a subcommand gives this argument as --query and --expect, too. */
bool acceptsQueryFlags(const ArgDef& arg)
{
  return arg.type == ArgType::Target && arg.name == QueryTargetArg;
}

Json opSchema(const OpDef& def)
{
  auto opName = Json::object();
  opName.set("const", def.name);

  auto properties = Json::object();
  properties.set("op", std::move(opName));

  auto required = Json::array();
  required.push("op");

  for (const auto& arg : def.args)
  {
    properties.set(arg.name, argSchema(arg));
    if (arg.required)
    {
      required.push(arg.name);
    }
  }

  auto result = Json::object();
  result.set("$schema", "https://json-schema.org/draft/2020-12/schema");
  result.set("type", "object");
  result.set("properties", std::move(properties));
  result.set("required", std::move(required));
  result.set("additionalProperties", false);
  return result;
}

Op parseOp(const Json& entry, const size_t index)
{
  if (!entry.isObject())
  {
    throw CliError{
      ErrorCode::InvalidBatch,
      fmt::format("op {} must be an object {{\"op\": <name>, ...}}", index)};
  }

  const auto* name = entry.find("op");
  if (!name || !name->isString())
  {
    throw invalidArgument(
      fmt::format("op {} must name its op as a string in \"op\"", index));
  }

  const auto* def = findOpDef(name->asString());
  if (!def)
  {
    throw invalidArgument(
      fmt::format("unknown op \"{}\"; tbcli ops --json lists them", name->asString()));
  }

  auto given = Json::object();
  for (const auto& member : entry.asObject())
  {
    if (member.key != "op")
    {
      given.set(member.key, member.value);
    }
  }
  return Op{def, checkArgs(*def, given)};
}

/** What transform ops accept: whatever can be moved, and layers for their contents. */
const auto TransformTargetRules = TargetRules{
  {TargetKind::Layer,
   TargetKind::Group,
   TargetKind::PointEntity,
   TargetKind::BrushEntity,
   TargetKind::Brush,
   TargetKind::Patch}};

/** What face ops accept: faces, and brushes for the faces that `faces` picks. */
const auto FaceTargetRules = TargetRules{{TargetKind::Brush, TargetKind::Face}, true};

/** The required Target argument `target`, which accepts what the rules say. */
ArgDef targetArg(const std::string_view description, const TargetRules& rules)
{
  return ArgDef{
    "target", ArgType::Target, std::string{description}, true, std::nullopt, rules};
}

ArgDef uvLockArg()
{
  return ArgDef{
    "uv_lock",
    ArgType::Bool,
    "keep the materials aligned to the moved faces",
    false,
    Json{true}};
}

ArgDef centerArg(const std::string_view description)
{
  return ArgDef{
    "center",
    ArgType::Vec3,
    fmt::format(
      "{}, in absolute units (default: the center of the targets' bounds)", description)};
}

ArgDef facesArg()
{
  return ArgDef{
    "faces",
    ArgType::FaceSelector,
    "which faces of the brush targets to act on: all (the default), or those whose "
    "normals are within 0.5 degrees of the given directions; not with face paths"};
}

/** What property ops accept: the world and entities, but not brushes. */
const auto PropertyTargetRules =
  TargetRules{{TargetKind::World, TargetKind::PointEntity, TargetKind::BrushEntity}};

std::vector<OpDef> makeOpDefs()
{
  return {
    OpDef{
      "create_box",
      "Creates an axis-aligned box brush from min to max, with one material on every "
      "face.",
      {
        ArgDef{"min", ArgType::Vec3, "the minimum corner, in absolute units", true},
        ArgDef{"max", ArgType::Vec3, "the maximum corner, in absolute units", true},
        ArgDef{"material", ArgType::String, "the material of every face", true},
        ArgDef{
          "parent",
          ArgType::NodePath,
          "the layer, group or brush entity to create the box in",
          false,
          Json{"layer:0"},
          {{TargetKind::Layer, TargetKind::Group, TargetKind::BrushEntity}}},
        ArgDef{"as", ArgType::Binding, "binds the created brush to this name"},
      },
      createBox,
    },
    OpDef{
      "create_point_entity",
      "Creates a point entity at an origin. The classname must be a point class of the "
      "FGD unless allow_undefined is set.",
      {
        ArgDef{"classname", ArgType::String, "the entity's classname", true},
        ArgDef{"origin", ArgType::Vec3, "the entity's origin, in absolute units", true},
        ArgDef{
          "properties",
          ArgType::Properties,
          "more properties, with string values; not classname or origin"},
        ArgDef{
          "allow_undefined",
          ArgType::Bool,
          "create the entity even if the FGD does not define the classname as a point "
          "class",
          false,
          Json{false}},
        ArgDef{
          "parent",
          ArgType::NodePath,
          "the layer or group to create the entity in",
          false,
          Json{"layer:0"},
          {{TargetKind::Layer, TargetKind::Group}}},
        ArgDef{"as", ArgType::Binding, "binds the created entity to this name"},
      },
      createPointEntity,
      checkCreatePointEntityArgs,
    },
    OpDef{
      "create_brush_entity",
      "Creates a brush entity and moves the brushes into it. The entity is created in "
      "the group or layer of the first brush. The classname must be a brush class of "
      "the FGD unless allow_undefined is set. The properties of the entities the "
      "brushes leave are not carried over; an entity left without brushes is deleted.",
      {
        ArgDef{"classname", ArgType::String, "the entity's classname", true},
        ArgDef{
          "brushes",
          ArgType::Target,
          "the brushes to move into the entity",
          true,
          std::nullopt,
          {{TargetKind::Brush}, true}},
        ArgDef{
          "properties",
          ArgType::Properties,
          "more properties, with string values; not classname"},
        ArgDef{
          "allow_undefined",
          ArgType::Bool,
          "create the entity even if the FGD does not define the classname as a brush "
          "class",
          false,
          Json{false}},
        ArgDef{"as", ArgType::Binding, "binds the created entity to this name"},
      },
      createBrushEntity,
      checkCreateBrushEntityArgs,
    },
    OpDef{
      "move_to_world",
      "Moves the brushes of brush entities to the world, into the group or layer that "
      "holds the entity. An entity left without brushes is deleted.",
      {
        ArgDef{
          "target",
          ArgType::Target,
          "the brush entities whose brushes to move, or brushes of brush entities; "
          "brushes already in the world stay where they are",
          true,
          std::nullopt,
          {{TargetKind::BrushEntity, TargetKind::Brush}}},
      },
      moveToWorld,
    },
    OpDef{
      "set_property",
      "Sets a property of entities or the world, adding it if it is missing. Values "
      "are strings; origin must be three numbers and angle a number. The classname "
      "cannot be changed.",
      {
        targetArg("the entities to set the property on", PropertyTargetRules),
        ArgDef{"key", ArgType::String, "the property's key", true},
        ArgDef{"value", ArgType::String, "the property's value", true},
      },
      setProperty,
      checkSetPropertyArgs,
    },
    OpDef{
      "remove_property",
      "Removes a property from entities or the world. Warns property_not_found if a "
      "target does not have it.",
      {
        targetArg("the entities to remove the property from", PropertyTargetRules),
        ArgDef{"key", ArgType::String, "the property's key", true},
      },
      removeProperty,
      checkRemovePropertyArgs,
    },
    OpDef{
      "rename_property",
      "Renames a property of entities or the world, keeping its value. Warns "
      "property_not_found if a target does not have it, and fails if a target has a "
      "property with the new key already.",
      {
        targetArg("the entities to rename the property of", PropertyTargetRules),
        ArgDef{"from", ArgType::String, "the property's key", true},
        ArgDef{"to", ArgType::String, "the property's new key", true},
      },
      renameProperty,
      checkRenamePropertyArgs,
    },
    OpDef{
      "set_spawnflag",
      "Sets or clears one spawnflag of entities. tbcli fgd <classname> lists the "
      "spawnflags of a class.",
      {
        targetArg("the entities to set the spawnflag of", PropertyTargetRules),
        ArgDef{
          "flag",
          ArgType::SpawnFlag,
          "the spawnflag: its name in the target's FGD class, case-insensitive, or its "
          "value, such as 256",
          true},
        ArgDef{"on", ArgType::Bool, "whether to set the spawnflag or clear it", true},
      },
      setSpawnflag,
    },
    OpDef{
      "translate",
      "Moves nodes by a delta. With uv_lock (the default), materials move with the "
      "faces.",
      {
        targetArg(
          "the nodes to move; a layer stands for the nodes in it", TransformTargetRules),
        ArgDef{"delta", ArgType::Vec3, "the distance to move, in units", true},
        uvLockArg(),
      },
      translate,
    },
    OpDef{
      "rotate",
      "Rotates nodes about an axis through a center, counterclockwise when looking "
      "against the axis. Point entities have their angle, angles or mangle property "
      "updated. With uv_lock (the default), materials turn with the faces.",
      {
        targetArg(
          "the nodes to rotate; a layer stands for the nodes in it",
          TransformTargetRules),
        ArgDef{"axis", ArgType::Axis, "the axis to rotate about", true},
        ArgDef{"degrees", ArgType::Number, "the angle, in degrees", true},
        centerArg("the point the axis goes through"),
        uvLockArg(),
      },
      rotate,
    },
    OpDef{
      "scale",
      "Scales nodes so that their bounds become \"to\", or by \"factors\" about a "
      "center; "
      "give one of the two. There is no shear. With uv_lock (the default), materials "
      "scale with the faces.",
      {
        targetArg(
          "the nodes to scale; a layer stands for the nodes in it", TransformTargetRules),
        ArgDef{"to", ArgType::Box, "the new bounds of the targets"},
        ArgDef{
          "factors", ArgType::Vec3, "the scale factors on the x, y and z axes; not 0"},
        centerArg("the point that stays in place when scaling by factors"),
        uvLockArg(),
      },
      scale,
      checkScaleArgs,
    },
    OpDef{
      "flip",
      "Mirrors nodes along an axis through a center. With uv_lock (the default), "
      "materials are mirrored with the faces.",
      {
        targetArg(
          "the nodes to flip; a layer stands for the nodes in it", TransformTargetRules),
        ArgDef{"axis", ArgType::Axis, "the axis to mirror along", true},
        centerArg("a point on the mirror plane"),
        uvLockArg(),
      },
      flip,
    },
    OpDef{
      "set_material",
      "Sets the material of faces. Warns missing_material if the map's materials can be "
      "checked (quake_dir is set) and the material is not among them, and "
      "no_faces_selected if faces picks no face of the brush targets.",
      {
        targetArg(
          "the faces, or the brushes whose faces to set the material of",
          FaceTargetRules),
        facesArg(),
        ArgDef{"material", ArgType::String, "the material's name", true},
      },
      setMaterial,
      checkSetMaterialArgs,
    },
    OpDef{
      "set_uv",
      "Sets the UV offset, scale and rotation of faces, each to a value or by adding "
      "{\"add\": ...} to it. Warns no_faces_selected if faces picks no face of the brush "
      "targets.",
      {
        targetArg(
          "the faces, or the brushes whose faces to set the UV of", FaceTargetRules),
        facesArg(),
        ArgDef{"offset", ArgType::PairOrAdd, "the offset in texels, [u, v]"},
        ArgDef{"scale", ArgType::PairOrAdd, "the scale, [u, v]; not 0"},
        ArgDef{"rotation", ArgType::NumberOrAdd, "the rotation, in degrees"},
      },
      setUv,
      checkSetUvArgs,
    },
    OpDef{
      "fit_uv",
      "Aligns, justifies and scales the UV of faces so that the material covers each "
      "face exactly once, as the editor's auto fit does. Needs the materials' sizes, so "
      "quake_dir must be set and the materials must be found. Warns no_faces_selected if "
      "faces picks no face of the brush targets.",
      {
        targetArg(
          "the faces, or the brushes whose faces to fit the UV of", FaceTargetRules),
        facesArg(),
      },
      fitUv,
    },
    OpDef{
      "reset_uv",
      "Resets the UV of faces to the game's defaults: offset and rotation 0, the default "
      "scale, and the face's default axes. Warns no_faces_selected if faces picks no "
      "face "
      "of the brush targets.",
      {
        targetArg(
          "the faces, or the brushes whose faces to reset the UV of", FaceTargetRules),
        facesArg(),
      },
      resetUv,
    },
    OpDef{
      "delete",
      "Deletes groups, entities, brushes and patches. An entity or group left empty "
      "is deleted with them.",
      {
        ArgDef{
          "target",
          ArgType::Target,
          "the nodes to delete",
          true,
          std::nullopt,
          {{TargetKind::Group,
            TargetKind::PointEntity,
            TargetKind::BrushEntity,
            TargetKind::Brush,
            TargetKind::Patch}}},
      },
      deleteNode,
    },
    OpDef{
      "fix",
      "Applies a quick-fix to the issues of one type on the target and inside it, or, "
      "without a target, to every issue of that type. tbcli ops --json lists the fix "
      "ids of each issue type. Issues on locked nodes are left out unless "
      "--allow-locked; issues that tbcli.json's verify.ignore suppresses are fixed "
      "too.",
      {
        ArgDef{"issue_type", ArgType::String, "the issue type, as verify names it", true},
        ArgDef{"fix", ArgType::String, "the id of the quick-fix", true},
        ArgDef{
          "target",
          ArgType::Target,
          "the nodes and faces whose issues to fix, with the nodes inside them; every "
          "issue of the type if not given",
          false,
          std::nullopt,
          {{TargetKind::World,
            TargetKind::Layer,
            TargetKind::Group,
            TargetKind::PointEntity,
            TargetKind::BrushEntity,
            TargetKind::Brush,
            TargetKind::Patch,
            TargetKind::Face}}},
      },
      fixIssues,
      checkFixArgs,
    },
  };
}

/**
 * Fails if the op refers to a binding that is not among the given ones, which earlier
 * ops bind.
 */
void checkBindingReferences(const Op& op, const std::set<std::string>& bindings)
{
  for (const auto& binding : op.bindingReferences())
  {
    if (!bindings.contains(binding))
    {
      throw invalidArgument(
        fmt::format(
          "{}: {} is not bound by an earlier op of the batch", op.def->name, binding));
    }
  }
}

} // namespace

const NodeOrFace& OpContext::node(const std::string_view name) const
{
  const auto& result = targets(name);
  contract_assert(result.size() == 1);
  return result.front();
}

const std::vector<NodeOrFace>& OpContext::targets(const std::string_view name) const
{
  const auto it = nodes.find(name);
  contract_assert(it != nodes.end());
  return it->second;
}

std::optional<std::string> Op::binding() const
{
  for (const auto& arg : def->args)
  {
    if (const auto* value = args.find(arg.name); value && arg.type == ArgType::Binding)
    {
      return value->asString();
    }
  }
  return std::nullopt;
}

bool isBoolFlag(const OpDef& def, const std::string_view flag)
{
  return std::ranges::any_of(def.args, [&](const auto& arg) {
    return arg.type == ArgType::Bool && flagName(arg.name) == flag;
  });
}

bool definesFlag(const OpDef& def, const std::string_view flag)
{
  return std::ranges::any_of(
    def.args, [&](const auto& arg) { return flagName(arg.name) == flag; });
}

bool addressesNodes(const ArgDef& arg)
{
  return arg.type == ArgType::NodePath || arg.type == ArgType::Target;
}

std::vector<std::string> Op::bindingReferences() const
{
  auto result = std::vector<std::string>{};
  for (const auto& arg : def->args)
  {
    if (const auto* value = args.find(arg.name); value && addressesNodes(arg))
    {
      std::ranges::move(cli::bindingReferences(*value), std::back_inserter(result));
    }
  }
  return result;
}

const std::vector<OpDef>& opDefs()
{
  static const auto defs = makeOpDefs();
  return defs;
}

const OpDef* findOpDef(const std::string_view name)
{
  const auto& defs = opDefs();
  const auto it = std::ranges::find(defs, name, &OpDef::name);
  return it != defs.end() ? &*it : nullptr;
}

Json opsJson()
{
  auto ops = Json::array();
  for (const auto& def : opDefs())
  {
    auto op = Json::object();
    op.set("name", def.name);
    op.set("description", def.description);
    op.set("schema", opSchema(def));
    ops.push(std::move(op));
  }

  auto result = Json::object();
  result.set("schema", "tbcli.ops/1");
  result.set("ops", std::move(ops));
  result.set("issue_types", issueTypesJson());
  return result;
}

std::string_view mutatingOptionsHelp()
{
  return R"(  --expect-rev <rev>    refuse to write unless the map's rev is <rev>
                        (stale_rev, exit 3)
  --dry-run             return the full result and the would-be rev without
                        writing
  -o <out.map>          write the result to out.map instead of the map
  --allow-locked        let ops edit locked nodes, and keep locked nodes in
                        query targets (locked)
  --allow-lossy         write even though the map did not load exactly
                        (lossy_load)
)";
}

std::string opHelp(const OpDef& def)
{
  auto result = fmt::format(
    "usage: tbcli {} <map> [options]\n\n{}\n\narguments:\n", def.name, def.description);
  for (const auto& arg : def.args)
  {
    const auto flag =
      fmt::format("--{} {}", flagName(arg.name), traits(arg.type).placeholder);
    const auto isQueryTarget = acceptsQueryFlags(arg);
    auto description =
      arg.required ? (isQueryTarget ? "required unless --query is given: " : "required: ")
                       + arg.description
                   : arg.description;
    if (arg.defaultValue)
    {
      description += fmt::format(
        " (default: {})",
        arg.defaultValue->isString() ? arg.defaultValue->asString()
                                     : arg.defaultValue->dump());
    }
    if (addressesNodes(arg))
    {
      description += fmt::format(" (accepts: {})", kindsHelp(arg));
    }
    result += flag.size() < 22 ? fmt::format("  {:<22}{}\n", flag, description)
                               : fmt::format("  {}\n  {:<22}{}\n", flag, "", description);
    if (isQueryTarget)
    {
      result += fmt::format(
        "  {:<22}instead of --{}: the nodes a TbQl query matches\n"
        "  {:<22}with --query: fail unless the query matches n nodes\n"
        "  {:<22}(default: any); locked nodes are left out\n",
        "--query <ql>",
        QueryTargetArg,
        "--expect <n | any>",
        "");
    }
  }
  result += "\noptions:\n";
  result += mutatingOptionsHelp();
  result += fmt::format(
    "\nIn an apply batch, the same op is {{\"op\": \"{}\", ...}} with the arguments "
    "as keys;\ntbcli ops --json prints its JSON Schema.\n",
    def.name);
  return result;
}

std::vector<Op> parseBatch(const Json& batch)
{
  if (!batch.isArray())
  {
    throw CliError{ErrorCode::InvalidBatch, "a batch must be a JSON array of ops"};
  }

  auto result = std::vector<Op>{};
  auto bindings = std::set<std::string>{};
  for (size_t i = 0; i < batch.size(); ++i)
  {
    attributeToOp(i, [&]() {
      auto op = parseOp(batch[i], i);
      checkBindingReferences(op, bindings);
      if (const auto binding = op.binding(); binding && !bindings.insert(*binding).second)
      {
        throw invalidArgument(
          fmt::format("{}: {} is bound by an earlier op", op.def->name, *binding));
      }
      result.push_back(std::move(op));
    });
  }
  return result;
}

Op parseOpFlags(const OpDef& def, const OpFlags& flags)
{
  const auto acceptsQuery = std::ranges::any_of(def.args, acceptsQueryFlags);

  auto given = Json::object();
  auto query = std::optional<std::string>{};
  auto expect = std::optional<std::string>{};
  for (const auto& [flag, value] : flags)
  {
    if (acceptsQuery && (flag == "query" || flag == "expect"))
    {
      auto& queryFlag = flag == "query" ? query : expect;
      if (queryFlag)
      {
        throw invalidArgument(fmt::format("{}: --{} is given twice", def.name, flag));
      }
      queryFlag = value;
      continue;
    }

    const auto it = std::ranges::find_if(
      def.args, [&](const auto& arg) { return flagName(arg.name) == flag; });
    if (it == def.args.end())
    {
      throw invalidArgument(fmt::format("{} does not accept --{}", def.name, flag));
    }
    if (given.find(it->name))
    {
      throw invalidArgument(fmt::format("{}: --{} is given twice", def.name, flag));
    }

    const auto& argTraits = traits(it->type);
    auto json = argTraits.fromFlag(value);
    if (!json)
    {
      throw invalidArgument(
        fmt::format(
          "{}: --{} must be {}, not '{}'", def.name, flag, argTraits.expected, value));
    }
    given.set(it->name, std::move(*json));
  }

  if (query || expect)
  {
    if (given.find(QueryTargetArg))
    {
      throw invalidArgument(
        fmt::format("{}: give either --{} or --query", def.name, QueryTargetArg));
    }
    given.set(std::string{QueryTargetArg}, queryTargetFromFlags(def.name, query, expect));
  }

  auto op = Op{&def, checkArgs(def, given)};
  checkBindingReferences(op, {});
  return op;
}

} // namespace tb::cli
