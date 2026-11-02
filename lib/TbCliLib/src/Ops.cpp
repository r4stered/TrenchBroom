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
#include "cli/ParseUtils.h"

#include "kd/contracts.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <set>

namespace tb::cli
{
namespace
{

CliError invalidArgument(const std::string& message)
{
  return CliError{ErrorCode::InvalidArgument, message};
}

bool isBindingName(const std::string_view name)
{
  const auto isAlpha = [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  const auto isAlnum = [&](const char c) { return isAlpha(c) || (c >= '0' && c <= '9'); };

  return name.size() >= 2 && name.front() == '$' && isAlpha(name[1])
         && std::ranges::all_of(name.substr(2), isAlnum);
}

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

Json bindingSchema()
{
  auto result = stringSchema();
  result.set("pattern", "^\\$[A-Za-z_][A-Za-z0-9_]*$");
  return result;
}

std::optional<Json> vec3FromFlag(const std::string& value)
{
  const auto numbers = parseNumberList(value);
  if (!numbers || numbers->size() != 3)
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

std::optional<Json> stringFromFlag(const std::string& value)
{
  return Json{value};
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

const auto ArgTypes = std::array<ArgTypeTraits, 4>{{
  {ArgType::Vec3,
   "<x,y,z>",
   "three numbers [x, y, z] (x,y,z as a flag)",
   isVec3,
   vec3Schema,
   vec3FromFlag},
  {ArgType::String, "<string>", "a string", isString, stringSchema, stringFromFlag},
  {ArgType::NodePath,
   "<path>",
   "a node path string",
   isString,
   stringSchema,
   stringFromFlag},
  {ArgType::Binding,
   "<$name>",
   "a binding name such as \"$room\"",
   isBinding,
   bindingSchema,
   stringFromFlag},
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
  return result;
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
          Json{"layer:0"}},
        ArgDef{"as", ArgType::Binding, "binds the created brush to this name"},
      },
      createBox,
    },
    OpDef{
      "delete",
      "Deletes a group, entity, brush or patch. An entity or group left empty is "
      "deleted with it.",
      {
        ArgDef{"target", ArgType::NodePath, "the node to delete", true},
      },
      deleteNode,
    },
  };
}

} // namespace

const NodeOrFace& OpContext::node(const std::string_view name) const
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
  return result;
}

std::string_view mutatingOptionsHelp()
{
  return R"(  --expect-rev <rev>    refuse to write unless the map's rev is <rev>
                        (stale_rev, exit 3)
  --dry-run             return the full result and the would-be rev without
                        writing
  -o <out.map>          write the result to out.map instead of the map
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
    auto description = arg.required ? "required: " + arg.description : arg.description;
    if (arg.defaultValue)
    {
      description += fmt::format(
        " (default: {})",
        arg.defaultValue->isString() ? arg.defaultValue->asString()
                                     : arg.defaultValue->dump());
    }
    result += flag.size() < 22 ? fmt::format("  {:<22}{}\n", flag, description)
                               : fmt::format("  {}\n  {:<22}{}\n", flag, "", description);
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
  auto given = Json::object();
  for (const auto& [flag, value] : flags)
  {
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

  return Op{&def, checkArgs(def, given)};
}

} // namespace tb::cli
