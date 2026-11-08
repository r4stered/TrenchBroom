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

#include "cli/Verify.h"

#include "cli/CappedList.h"
#include "cli/CliError.h"
#include "cli/CliValidators.h"
#include "cli/Config.h"
#include "cli/GeometryJson.h"
#include "cli/Leak.h"
#include "cli/MapRef.h"
#include "cli/NodePath.h"
#include "cli/Query.h"
#include "cli/Sha256.h"
#include "cli/Timing.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Issue.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/NodeWriter.h"
#include "mdl/PatchNode.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include "kd/contracts.h"
#include "kd/overload.h"
#include "kd/ranges/to.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace tb::cli
{
namespace
{

constexpr auto KeyLength = size_t{12};

/**
 * Restores the file positions of a node, everything below it and their faces when it
 * goes out of scope. The map writer sets the positions of what it writes to their lines
 * in its output, which must not replace the lines in the loaded file.
 */
class FilePositionGuard
{
private:
  struct NodePosition
  {
    const mdl::Node* node;
    size_t lineNumber;
    size_t lineCount;
  };

  struct FacePosition
  {
    const mdl::BrushFace* face;
    size_t lineNumber;
  };

  std::vector<NodePosition> m_nodes;
  std::vector<FacePosition> m_faces;

public:
  explicit FilePositionGuard(const mdl::Node& node) { record(node); }

  ~FilePositionGuard()
  {
    for (const auto& [node, lineNumber, lineCount] : m_nodes)
    {
      node->setFilePosition(lineNumber, lineCount);
    }
    for (const auto& [face, lineNumber] : m_faces)
    {
      // a face is always written on one line
      face->setFilePosition(lineNumber, 1);
    }
  }

  FilePositionGuard(const FilePositionGuard&) = delete;
  FilePositionGuard& operator=(const FilePositionGuard&) = delete;

private:
  void record(const mdl::Node& node)
  {
    m_nodes.push_back({&node, node.lineNumber(), node.lineCount()});
    if (const auto* brushNode = dynamic_cast<const mdl::BrushNode*>(&node))
    {
      for (const auto& face : brushNode->brush().faces())
      {
        m_faces.push_back({&face, face.lineNumber()});
      }
    }
    for (const auto* child : node.children())
    {
      record(*child);
    }
  }
};

/**
 * The text that identifies a node's content: what the map writer writes for it. A
 * brush is written as its faces only, so that its text does not depend on worldspawn,
 * and the world as its properties only, so that its text does not depend on its
 * brushes. Link ids are left out: the editor makes up a random one for every group
 * that has none in the file, so they would change on every load until the map is saved.
 */
std::string serializedText(
  const mdl::WorldNode& worldNode, mdl::Node& node, kdl::task_manager& taskManager)
{
  auto stream = std::ostringstream{};
  auto writer = mdl::NodeWriter{worldNode, stream};
  const auto writeNode = [&](mdl::Node& n) {
    const auto guard = FilePositionGuard{n};
    writer.writeNodes({&n}, taskManager);
  };

  node.accept(
    kdl::overload(
      [&](mdl::WorldNode& world) {
        for (const auto& property : world.entity().properties())
        {
          stream << '"' << property.key() << "\" \"" << property.value() << "\"\n";
        }
      },
      [&](mdl::LayerNode& layer) { stream << layer.name(); },
      [&](mdl::GroupNode& group) { writeNode(group); },
      [&](mdl::EntityNode& entity) { writeNode(entity); },
      [&](mdl::BrushNode& brush) {
        const auto guard = FilePositionGuard{brush};
        writer.writeBrushFaces(brush.brush().faces(), taskManager);
      },
      [&](mdl::PatchNode& patch) { writeNode(patch); }));

  const auto linkIdLine = fmt::format("\"{}\"", mdl::EntityPropertyKeys::TbLinkId);
  const auto text = stream.str();
  auto result = std::string{};
  for (const auto range : std::views::split(text, '\n'))
  {
    const auto line = std::string_view{range.begin(), range.end()};
    if (!line.starts_with(linkIdLine))
    {
      result += line;
      result += '\n';
    }
  }
  return result;
}

/**
 * Gives every issue its key. Issues of the same type on the same node are told apart by
 * their face or property. Any that are still alike, such as two identical brushes with
 * the same issue, are told apart by their order in the map, so their keys change when an
 * identical node before them is added or removed.
 */
class KeyMaker
{
private:
  const mdl::WorldNode& m_worldNode;
  kdl::task_manager& m_taskManager;
  std::unordered_map<const mdl::Node*, std::string> m_texts;
  std::unordered_map<std::string, size_t> m_seen;

public:
  KeyMaker(const mdl::WorldNode& worldNode, kdl::task_manager& taskManager)
    : m_worldNode{worldNode}
    , m_taskManager{taskManager}
  {
  }

  /** The key of an issue of the given type that is identified by the given text. */
  std::string key(const std::string_view type, const std::string& text)
  {
    auto input = fmt::format("{}\n{}", type, text);
    if (const auto ordinal = m_seen[input]++; ordinal > 0)
    {
      input += fmt::format("\n#{}", ordinal);
    }
    return sha256Hex(input).substr(0, KeyLength);
  }

  /**
   * The key of an issue of the given type on the given node, which is told apart from
   * other issues of that type on the node by the given detail.
   */
  std::string key(
    const std::string_view type, mdl::Node& node, const std::string_view detail)
  {
    return key(type, fmt::format("{}\n{}", textOf(node), detail));
  }

  /** The key of a validator issue of the given type, on the given face if it has one. */
  std::string key(
    const std::string_view type,
    const mdl::Issue& issue,
    const std::optional<size_t> faceIndex)
  {
    auto text = textOf(issue.node());
    if (faceIndex)
    {
      text += fmt::format("\nface:{}", *faceIndex);
    }
    else if (
      const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(&issue))
    {
      text += fmt::format("\nproperty:{}", propertyIssue->propertyKey());
    }
    return key(type, text);
  }

private:
  const std::string& textOf(mdl::Node& node)
  {
    auto [it, inserted] = m_texts.try_emplace(&node);
    if (inserted)
    {
      it->second = serializedText(m_worldNode, node, m_taskManager);
    }
    return it->second;
  }
};

void collectLoadErrors(
  const MapRef& mapRef, KeyMaker& keys, std::vector<VerifyIssue>& issues)
{
  const auto& type = *findIssueType("load_error");
  for (const auto& problem : mapRef.loadProblems)
  {
    issues.push_back(
      VerifyIssue{
        .type = &type,
        .severity = type.severity,
        .key = keys.key(type.id, problem.message),
        .message = problem.message,
        .line = problem.line,
      });
  }
}

/** Runs validators on nodes and collects their issues. */
class ValidatorIssueCollector
{
private:
  std::vector<const mdl::Validator*> m_validators;
  std::unordered_map<mdl::IssueType, const IssueTypeDef*> m_types;
  KeyMaker& m_keys;
  std::vector<VerifyIssue>& m_issues;

public:
  ValidatorIssueCollector(
    std::vector<const mdl::Validator*> validators,
    KeyMaker& keys,
    std::vector<VerifyIssue>& issues)
    : m_validators{std::move(validators)}
    , m_keys{keys}
    , m_issues{issues}
  {
    for (const auto* validator : m_validators)
    {
      const auto* type = findIssueType(*validator);
      if (!type)
      {
        throw CliError{
          ErrorCode::InternalError,
          fmt::format(
            "the validator '{}' has no entry in the issue-type table",
            validator->description())};
      }
      m_types[validator->type()] = type;
    }
  }

  /**
   * Collects the issues of the given node and everything below it, depth first. The
   * validators run on each node directly rather than through the node's issue cache,
   * which holds the issues of whichever validators ran first.
   */
  void collect(mdl::Node& node)
  {
    collectNode(node);
    for (auto* child : node.children())
    {
      collect(*child);
    }
  }

  /** Collects the issues of the given node only. */
  void collectNode(mdl::Node& node)
  {
    auto issues = std::vector<std::unique_ptr<mdl::Issue>>{};
    for (const auto* validator : m_validators)
    {
      validator->validate(node, issues);
    }
    for (const auto& issue : issues)
    {
      add(node, *issue);
    }
  }

private:
  void add(mdl::Node& node, const mdl::Issue& issue)
  {
    const auto* type = m_types.at(issue.type());
    const auto onNode = type->scope == IssueScope::Node;
    const auto* faceIssue = dynamic_cast<const mdl::BrushFaceIssue*>(&issue);
    const auto faceIndex =
      faceIssue ? std::optional{faceIssue->faceIndex()} : std::nullopt;
    const auto* propertyIssue = dynamic_cast<const mdl::EntityPropertyIssue*>(&issue);
    const auto line = issue.lineNumber();
    m_issues.push_back(
      VerifyIssue{
        .type = type,
        .severity = type->severity,
        .key = onNode ? m_keys.key(type->id, issue, faceIndex)
                      : m_keys.key(type->id, issue.description()),
        .message = issue.description(),
        .node = onNode ? &node : nullptr,
        .faceIndex = onNode ? faceIndex : std::nullopt,
        .propertyKey = onNode && propertyIssue
                         ? std::optional{propertyIssue->propertyKey()}
                         : std::nullopt,
        .line = onNode && line > 0 ? std::optional{line} : std::nullopt,
      });
  }
};

/** The innermost node whose lines contain the given line, or nullptr. */
mdl::Node* nodeAtLine(mdl::Node& node, const size_t line)
{
  // the default layer has no lines of its own, so the children of a node that does not
  // contain the line are searched, too
  auto* result = node.containsLine(line) ? &node : nullptr;
  for (auto* child : node.children())
  {
    if (auto* inner = nodeAtLine(*child, line))
    {
      return inner;
    }
  }
  return result;
}

/**
 * The first entity in file order of the given class whose bounds contain the given
 * point, or nullptr.
 */
mdl::Node* entityAt(mdl::Node& node, const std::string& classname, const vm::vec3d& point)
{
  if (
    auto* entityNode = dynamic_cast<mdl::EntityNode*>(&node);
    entityNode && entityNode->entity().classname() == classname
    && entityNode->logicalBounds().contains(point))
  {
    return entityNode;
  }
  for (auto* child : node.children())
  {
    if (auto* found = entityAt(*child, classname, point))
    {
      return found;
    }
  }
  return nullptr;
}

/** The node a compiler message is about, if it names one that the map has. */
mdl::Node* nodeOf(mdl::WorldNode& worldNode, const CompilerMessage& message)
{
  if (message.line)
  {
    return nodeAtLine(worldNode, *message.line);
  }
  if (message.classname && message.origin)
  {
    return entityAt(worldNode, *message.classname, *message.origin);
  }
  return nullptr;
}

/** How many brushes the `leak` issue lists. */
constexpr auto LeakBrushCount = size_t{3};

/**
 * The `leak` issue of a leaking compile, on the occupant entity that the first of
 * qbsp's `Reached occupant` lines names.
 */
VerifyIssue leakIssue(
  const CompileResult& compileResult,
  mdl::WorldNode& worldNode,
  const bool lossy,
  KeyMaker& keys)
{
  const auto& leakType = *findIssueType("leak");
  // qbsp always runs first
  const auto& qbspMessages = compileResult.runs.front().messages;
  const auto occupant = std::ranges::find_if(qbspMessages, isLeakOccupant);
  auto* node = occupant != qbspMessages.end() ? nodeOf(worldNode, *occupant) : nullptr;
  const auto message =
    occupant != qbspMessages.end() && occupant->classname && occupant->origin
      ? fmt::format(
          "the map leaks: the void can be reached from {} at ({} {} {})",
          *occupant->classname,
          occupant->origin->x(),
          occupant->origin->y(),
          occupant->origin->z())
      : std::string{"the map leaks"};

  return VerifyIssue{
    .type = &leakType,
    .severity = leakType.severity,
    .key = node ? keys.key(leakType.id, *node, QbspTool)
                : keys.key(leakType.id, std::string{QbspTool}),
    .message = message,
    .node = node,
    .line =
      node && node->lineNumber() > 0 ? std::optional{node->lineNumber()} : std::nullopt,
    .tool = std::string{QbspTool},
    .unreliableMapping = lossy,
    .leak =
      LeakDetails{
        .trail = simplifyTrail(compileResult.leakTrail),
        .nearestBrushes =
          brushesNearExit(worldNode, compileResult.leakTrail, LeakBrushCount),
      },
  };
}

/**
 * Turns the messages of the compilers into `compiler` issues, a leak into a `leak`
 * issue and every failed compiler into a `compile_fatal` issue.
 */
void collectCompilerIssues(
  const CompileResult& compileResult,
  const MapRef& mapRef,
  KeyMaker& keys,
  std::vector<VerifyIssue>& issues)
{
  const auto& compilerType = *findIssueType("compiler");
  const auto& fatalType = *findIssueType("compile_fatal");
  const auto lossy = !mapRef.loadProblems.empty();
  auto& worldNode = mapRef.map.worldNode();

  for (const auto& run : compileResult.runs)
  {
    const auto leakReported = compileResult.leaked && run.tool == QbspTool;
    if (leakReported)
    {
      issues.push_back(leakIssue(compileResult, worldNode, lossy, keys));
    }

    for (const auto& message : run.messages)
    {
      if (leakReported && isLeakOccupant(message))
      {
        // the leak issue reports these
        continue;
      }

      auto* node = nodeOf(worldNode, message);
      const auto detail =
        fmt::format("{}\n{}", run.tool, withoutLineNumbers(message.message));
      const auto nodeLine =
        node && node->lineNumber() > 0 ? std::optional{node->lineNumber()} : std::nullopt;
      issues.push_back(
        VerifyIssue{
          .type = &compilerType,
          .severity = message.severity,
          .key = node ? keys.key(compilerType.id, *node, detail)
                      : keys.key(compilerType.id, detail),
          .message = message.message,
          .node = node,
          .line = message.line ? message.line : nodeLine,
          .tool = run.tool,
          .unreliableMapping = lossy,
        });
    }

    if (run.failed())
    {
      const auto message = run.failureMessage();
      issues.push_back(
        VerifyIssue{
          .type = &fatalType,
          .severity = fatalType.severity,
          .key = keys.key(fatalType.id, fmt::format("{}\n{}", run.tool, message)),
          .message = message,
          .tool = run.tool,
        });
    }
  }
}

std::vector<const mdl::Validator*> pointers(
  const std::vector<std::unique_ptr<mdl::Validator>>& validators)
{
  return validators | std::views::transform(&std::unique_ptr<mdl::Validator>::get)
         | kdl::ranges::to<std::vector<const mdl::Validator*>>();
}

/** How long the function takes to run, in milliseconds. */
template <typename F>
double timed(const F& function)
{
  const auto start = std::chrono::steady_clock::now();
  function();
  return msSince(start);
}

/** The nodes and faces a path or query names. */
struct Selection
{
  std::set<const mdl::Node*> nodes;
  std::set<std::pair<const mdl::Node*, size_t>> faces;

  /** Whether the issue is on a selected node or face, or inside a selected node. */
  bool contains(const VerifyIssue& issue) const
  {
    if (issue.faceIndex && faces.contains({issue.node, *issue.faceIndex}))
    {
      return true;
    }
    for (const auto* node = issue.node; node; node = node->parent())
    {
      if (nodes.contains(node))
      {
        return true;
      }
    }
    return false;
  }
};

Selection selectionOf(const std::vector<NodeOrFace>& matches)
{
  auto result = Selection{};
  for (const auto& match : matches)
  {
    if (match.faceIndex)
    {
      result.faces.emplace(match.node, *match.faceIndex);
    }
    else
    {
      result.nodes.insert(match.node);
    }
  }
  return result;
}

/**
 * Whether a `--target` value is a node path rather than a TbQl query: paths start with
 * `world`, `layer:` or `group:`, which no query does.
 */
bool isNodePath(const std::string_view target)
{
  return target == WorldPath || target.starts_with("layer:")
         || target.starts_with("group:");
}

Selection selectTarget(mdl::Map& map, const std::string& target)
{
  return isNodePath(target) ? selectionOf({resolveNodePath(map.worldNode(), target)})
                            : selectionOf(queryMatches(map, target));
}

} // namespace

void applyConfig(
  mdl::Map& map, const VerifyConfig& config, std::vector<VerifyIssue>& issues)
{
  for (auto& issue : issues)
  {
    if (const auto it = config.severity.find(issue.type); it != config.severity.end())
    {
      issue.severity = it->second;
    }
  }

  for (const auto& suppression : config.ignore)
  {
    const auto selection =
      suppression.query
        ? std::optional{selectionOf(queryMatches(map, *suppression.query))}
        : std::nullopt;
    for (auto& issue : issues)
    {
      if (
        issue.type == suppression.type
        && (!suppression.key || issue.key == *suppression.key)
        && (!selection || selection->contains(issue)))
      {
        issue.suppressed = true;
      }
    }
  }
}

namespace
{

template <typename P>
bool hasUnsuppressedIssue(const std::vector<VerifyIssue>& issues, const P& predicate)
{
  return std::ranges::any_of(
    issues, [&](const auto& issue) { return !issue.suppressed && predicate(issue); });
}

std::string_view statusOf(const VerifyStage& stage)
{
  if (stage.skipReason)
  {
    return "skipped";
  }
  if (stage.failed)
  {
    return "failed";
  }
  return stage.hasIssues ? "issues" : "ok";
}

Json stageToJson(const VerifyStage& stage)
{
  auto result = Json::object();
  result.set("name", stage.name);
  result.set("status", statusOf(stage));
  if (stage.skipReason)
  {
    result.set("reason", *stage.skipReason);
  }
  if (!stage.unchecked.empty())
  {
    auto unchecked = Json::array();
    for (const auto* type : stage.unchecked)
    {
      unchecked.push(type->id);
    }
    result.set("unchecked", std::move(unchecked));
  }
  result.set("ms", std::round(stage.ms));
  return result;
}

void setLocation(Json& result, const VerifyIssue& issue)
{
  issue.node->accept(
    kdl::overload(
      [](const mdl::WorldNode&) {},
      [](const mdl::LayerNode&) {},
      [&](const mdl::GroupNode& group) {
        // an empty group has no bounds
        if (group.hasChildren())
        {
          result.set("bounds", toJson(group.logicalBounds()));
        }
      },
      [&](const mdl::EntityNode& entity) {
        if (entity.hasChildren())
        {
          result.set("bounds", toJson(entity.logicalBounds()));
        }
        else
        {
          result.set("origin", toJson(entity.entity().origin()));
        }
      },
      [&](const mdl::BrushNode& brush) {
        result.set(
          "bounds",
          toJson(
            issue.faceIndex ? brush.brush().face(*issue.faceIndex).bounds()
                            : brush.logicalBounds()));
      },
      [&](const mdl::PatchNode& patch) {
        result.set("bounds", toJson(patch.logicalBounds()));
      }));
}

/**
 * Sets `trail: {start, end, turning_points}`, or null without a trail, and
 * `nearest_brushes`.
 */
void setLeakDetails(
  Json& result,
  const LeakDetails& leak,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  if (leak.trail.empty())
  {
    result.set("trail", nullptr);
  }
  else
  {
    auto turningPoints = Json::array();
    // the points between the endpoints
    for (size_t i = 1; i + 1 < leak.trail.size(); ++i)
    {
      turningPoints.push(toJson(leak.trail[i]));
    }
    auto trail = Json::object();
    trail.set("start", toJson(leak.trail.front()));
    trail.set("end", toJson(leak.trail.back()));
    trail.set("turning_points", std::move(turningPoints));
    result.set("trail", std::move(trail));
  }

  auto brushes = Json::array();
  for (const auto* brush : leak.nearestBrushes)
  {
    brushes.push(paths.at(brush));
  }
  result.set("nearest_brushes", std::move(brushes));
}

} // namespace

Json issueToJson(
  const VerifyIssue& issue,
  const std::unordered_map<const mdl::Node*, std::string>& paths)
{
  auto result = Json::object();
  result.set("type", issue.type->id);
  result.set("severity", toString(issue.severity));
  result.set("key", issue.key);
  result.set("message", issue.message);

  if (issue.node)
  {
    const auto& path = paths.at(issue.node);
    result.set(
      "node", issue.faceIndex ? fmt::format("{}/face:{}", path, *issue.faceIndex) : path);
  }
  else
  {
    result.set("node", nullptr);
  }
  result.set("line", issue.line ? Json{*issue.line} : Json{nullptr});

  if (issue.node)
  {
    setLocation(result, issue);
  }
  if (issue.tool)
  {
    result.set("tool", *issue.tool);
  }
  if (issue.unreliableMapping)
  {
    result.set("unreliable_mapping", true);
  }
  if (issue.leak)
  {
    setLeakDetails(result, *issue.leak, paths);
  }

  result.set("fixes", fixIdsJson(*issue.type));
  return result;
}

Json leakIssueToJson(
  const MapRef& mapRef, const VerifyConfig& config, const CompileResult& compileResult)
{
  auto& map = mapRef.map;
  auto& worldNode = map.worldNode();
  auto keys = KeyMaker{worldNode, map.taskManager()};
  auto issues = std::vector<VerifyIssue>{
    leakIssue(compileResult, worldNode, !mapRef.loadProblems.empty(), keys)};
  applyConfig(map, config, issues);
  return issueToJson(issues.front(), nodePaths(worldNode));
}

namespace
{

/** The unsuppressed issues the list filters keep, in file order. */
std::vector<const VerifyIssue*> filterIssues(
  mdl::Map& map, const std::vector<VerifyIssue>& issues, const VerifyOptions& options)
{
  const auto target =
    options.target ? std::optional{selectTarget(map, *options.target)} : std::nullopt;

  auto result = std::vector<const VerifyIssue*>{};
  for (const auto& issue : issues)
  {
    if (
      !issue.suppressed
      && (options.types.empty()
          || std::ranges::find(options.types, issue.type) != options.types.end())
      && (!options.minSeverity || issue.severity >= *options.minSeverity)
      && (!target || target->contains(issue)))
    {
      result.push_back(&issue);
    }
  }
  return result;
}

} // namespace

IssueReport collectIssues(
  const MapRef& mapRef,
  const VerifyConfig& config,
  const std::optional<CompileOptions>& compileOptions,
  const std::optional<MapSource>& source)
{
  contract_pre(!compileOptions || source);

  auto& map = mapRef.map;
  auto& worldNode = map.worldNode();
  auto keys = KeyMaker{worldNode, map.taskManager()};
  auto result = IssueReport{};

  collectLoadErrors(mapRef, keys, result.issues);

  const auto validatorMs = timed([&] {
    ValidatorIssueCollector{worldNode.registeredValidators(), keys, result.issues}
      .collect(worldNode);
  });

  const auto materialCheck = materialCheckOf(map);
  const auto cliMs = timed([&] {
    const auto cliValidators = makeCliValidators(materialCheck);
    ValidatorIssueCollector{pointers(cliValidators), keys, result.issues}.collect(
      worldNode);
  });

  const auto compileResult =
    compileOptions ? std::optional{compile(
                       source->bytes, mapName(mapRef), mapRef.rev, *compileOptions)}
                   : std::nullopt;
  if (compileResult)
  {
    collectCompilerIssues(*compileResult, mapRef, keys, result.issues);
  }

  // validator issues are collected depth first; the lines give the file order, with
  // issues that have no line first, and on a line, load errors before validator issues
  // before CLI check issues before compiler issues
  std::ranges::stable_sort(
    result.issues, {}, [](const auto& issue) { return issue.line.value_or(0); });

  applyConfig(map, config, result.issues);

  const auto hasIssuesFrom = [&](const IssueSource source) {
    return hasUnsuppressedIssue(result.issues, [&](const VerifyIssue& issue) {
      return issue.type->source == source;
    });
  };
  result.stages = {
    {"load_diagnostics",
     hasIssuesFrom(IssueSource::LoadDiagnostics),
     source ? source->loadMs : 0.0},
    {"validators", hasIssuesFrom(IssueSource::Validator), validatorMs},
    {"cli_checks", hasIssuesFrom(IssueSource::CliCheck), cliMs},
  };
  if (materialCheck == MaterialCheck::Off)
  {
    result.stages.back().unchecked.push_back(findIssueType("missing_material"));
  }

  if (compileResult)
  {
    for (const auto& run : compileResult->runs)
    {
      result.stages.push_back(
        {.name = run.tool,
         .hasIssues = hasUnsuppressedIssue(
           result.issues,
           [&](const VerifyIssue& issue) { return issue.tool == run.tool; }),
         .ms = run.ms,
         .failed = run.failed()});
    }
    for (const auto& tool : compileResult->skipped)
    {
      result.stages.push_back({.name = tool, .skipReason = "leak"});
    }
  }
  return result;
}

struct NodeIssueCollector::Impl
{
  KeyMaker keys;
  std::vector<VerifyIssue> collected;
  ValidatorIssueCollector collector;

  Impl(mdl::Map& map, std::vector<const mdl::Validator*> validators)
    : keys{map.worldNode(), map.taskManager()}
    , collector{std::move(validators), keys, collected}
  {
  }
};

NodeIssueCollector::NodeIssueCollector(
  mdl::Map& map, std::vector<const mdl::Validator*> validators)
  : m_impl{std::make_unique<Impl>(map, std::move(validators))}
{
}

NodeIssueCollector::~NodeIssueCollector() = default;

std::vector<VerifyIssue> NodeIssueCollector::collect(mdl::Node& node)
{
  m_impl->collector.collectNode(node);
  return std::exchange(m_impl->collected, {});
}

void setIssueCounts(Json& object, const std::vector<VerifyIssue>& issues)
{
  auto counts = Json::object();
  for (const auto& type : issueTypes())
  {
    const auto count = std::ranges::count_if(issues, [&](const auto& issue) {
      return !issue.suppressed && issue.type == &type;
    });
    if (count > 0)
    {
      counts.set(std::string{type.id}, size_t(count));
    }
  }
  counts.set(
    "suppressed", size_t(std::ranges::count_if(issues, &VerifyIssue::suppressed)));

  auto bySeverity = Json::object();
  for (const auto severity : {Severity::Error, Severity::Warning, Severity::Info})
  {
    bySeverity.set(
      std::string{toString(severity)},
      size_t(std::ranges::count_if(issues, [&](const auto& issue) {
        return !issue.suppressed && issue.severity == severity;
      })));
  }

  object.set("counts", std::move(counts));
  object.set("by_severity", std::move(bySeverity));
}

VerifyResult verify(
  const MapRef& mapRef,
  const VerifyConfig& config,
  const VerifyOptions& options,
  const std::optional<MapSource>& source)
{
  auto& map = mapRef.map;
  const auto report = collectIssues(mapRef, config, options.compile, source);
  const auto listed = filterIssues(map, report.issues, options);

  const auto threshold = options.failOn.value_or(Severity::Error);
  const auto failing = hasUnsuppressedIssue(
    report.issues, [&](const auto& issue) { return issue.severity >= threshold; });

  const auto paths = nodePaths(map.worldNode());
  auto issues = page(listed, options.paging, [&](const VerifyIssue* issue) {
    return issueToJson(*issue, paths);
  });

  auto stages = Json::array();
  for (const auto& stage : report.stages)
  {
    stages.push(stageToJson(stage));
  }

  auto result = Json::object();
  result.set("schema", "tbcli.verify/1");
  result.set("rev", mapRef.rev);
  result.set("ok", !failing);
  result.set("stages", std::move(stages));
  setIssueCounts(result, report.issues);
  result.set("issues", std::move(issues));
  return {std::move(result), failing && options.failOn.has_value()};
}

} // namespace tb::cli
