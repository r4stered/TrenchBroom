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

#include "cli/Guide.h"

#include "cli/CliError.h"
#include "cli/FaceOps.h"
#include "cli/FileUtils.h"
#include "cli/Ops.h"
#include "cli/Targets.h"

#include "kd/string_utils.h"

#include <fmt/format.h>
#include <fmt/std.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

namespace tb::cli
{
namespace
{

/*
 * The guide's text. Placeholders in braces, such as {topics}, are filled in from the
 * definitions they name, so that the lists cannot drift from what tbcli accepts.
 */

constexpr auto OverviewText = R"(# tbcli guide

tbcli reads, edits, generates and verifies Quake `.map` files in the Valve 220 format.
Every call is stateless: it loads the map, acts and, if it mutates, writes the file back
atomically. The file is the only state. Results are JSON on stdout, tagged with a
`schema` and the map's `rev`; errors are `{"error": {"code", "message", ...}}`. Exit
codes: 0 ok, 1 failed and nothing written, 2 bad arguments, 3 stale rev, 4 `verify
--fail-on` found issues.

This guide never repeats the reference; read it there:

- `tbcli --help` lists the commands and options, `tbcli <op> --help` one op's arguments.
- `tbcli ops --json` has every op's JSON Schema (`ops`) and every issue type with its
  default severity and fix ids (`issue_types`). Build batches from it.

## The loop

1. **Orient:** `tbcli describe <map>`. Done when you know the map's bounds, layers and
   groups, key entities and built rooms, and have its `rev`.
2. **Find:** `tbcli query <map> '<ql>'` and `tbcli show <map> <path>`. Done when every
   node your change touches is a path from this rev, or a query whose match count you
   know.
3. **Rehearse:** write the ops as a JSON array and run `tbcli apply <map> batch.json
   --expect-rev <rev> --dry-run`. Done when every op is `ok` and its `created`,
   `modified`, `removed` and `warnings` are what you meant. Nothing is written.
4. **Apply:** the same command without `--dry-run`. Done when it exits 0. The result's
   `rev` is the new rev; its `created` and `modified` paths and `bindings` are against
   it, `removed` is against the old rev.
5. **Check the delta:** read each op's `issues_introduced`. Done when it is empty, or
   every issue in it is one your next batch fixes.
6. **Verify:** `tbcli verify <map>`. Done when `ok` is true. Before you call a change
   done, run `tbcli verify <map> --compile`: done when `ok` is true and there is no
   `leak` issue, which means the map is sealed.
7. **Repeat** from step 2 with the new rev until `verify --compile` is `ok`.

A batch is all or nothing: if an op fails, nothing is written and the error names it by
`op_index`. Every op is also a subcommand, as in `tbcli translate <map> --target <path>
--delta 0,0,16 --expect-rev <rev>`. A batch:

```json
[{"op": "create_box", "min": [0, 0, 0], "max": [256, 256, 16],
  "material": "ground1_6", "as": "$floor"},
 {"op": "translate", "target": "$floor", "delta": [0, 0, -16]}]
```

## Rules

- **Paths die with the rev.** A node path such as `layer:0/group:1/brush:3` is a
  position in one rev of the file and means nothing in any other. After a write, chain
  from the paths and `bindings` the result returns, or query again; never reuse old
  paths. Inside a batch, name what an op creates with `"as": "$name"` and refer to it
  as `"$name"`: created nodes have no path until the batch is written.
- **Always pass `--expect-rev <rev>`** to `apply` and every op. If the file changed
  since you read it, tbcli refuses with `stale_rev` (exit 3) and writes nothing. Read
  the map again; don't retry blindly.
- **Set `expect` on query targets:** `{"query": "<ql>", "expect": 3}`, or `--query
  '<ql>' --expect 3`. Without it any number of matches, none included, is accepted;
  with it a different count fails with `expect_mismatch`. Count first with `tbcli query
  <map> '<ql>' --count-only`.
- **Quote TbQl strings:** `classname is "light"`, not `classname is light`. A query that
  does not parse, or that refers to unknown names, is silently run as a text search
  for its own text and usually matches nothing.
- **A lossy load** means the reader skipped content it could not parse: a face, brush,
  layer or group. `verify` lists each skipped item as a `load_error` issue with its
  `line`. Writing would drop that content for good, so `apply` and every op refuse with
  `lossy_load`; `--dry-run` still works. Fix the file text at those lines, or pass
  `--allow-lossy` only if the user accepts the loss. Compiler issues after a lossy load
  are marked `unreliable_mapping`.
- **Locked nodes** are refused (`locked`) and left out of the matches of query targets.
  There is no unlock op; pass `--allow-locked` only when the user asks for it.
- **Sealed means done.** A map that loads and passes `verify` can still leak: qbsp
  finds a way from an entity into the void, vis and light are skipped, and the map does
  not play as built. A change is done only when `tbcli verify <map> --compile` is `ok`
  and has no `leak` issue.
- Coordinates are absolute Quake units with z up, and nothing is snapped for you. Undo
  is git, or write the result elsewhere with `-o <out.map>`.
- Change maps through tbcli rather than by editing the `.map` text, except to repair
  what a lossy load skipped.

## Topics

Load a topic with `tbcli guide <topic>` when your task needs it:

{topics}
## Ops

Arguments in `tbcli <op> --help`, schemas in `tbcli ops --json`:

{ops}
)";

constexpr auto ConventionsText = R"(# Conventions

Build spaces a player can move through. Sizes are in Quake units; z is up. {source}

{table}{notes})";

/** How to use the default conventions; a project's own table may not have these rows. */
constexpr auto DefaultConventionsNotes = R"(
A player fits through a space only if it is larger than the player hull on every axis,
climbs steps no higher than the step height, and cannot reach ledges above the jump
height. Keep brushwork on the grid, so that faces meet without gaps.
)";

constexpr auto DefaultConventionsSource =
  "These are the Quake 1 defaults; a project can replace them with `guide.conventions` "
  "in tbcli.json.";

constexpr auto ConfiguredConventionsSource =
  "These come from `guide.conventions` in the project's tbcli.json.";

const auto DefaultConventions = std::vector<Convention>{
  {"Player hull", "32 x 32 x 56"},
  {"Player origin", "24 above the floor (the hull spans -24..+32 on z)"},
  {"Step height", "18"},
  {"Jump height", "about 45"},
  {"Corridor", "at least 64 wide x 96 tall"},
  {"Door", "64 x 96"},
  {"Grid", "powers of two; 16 for brushwork"},
  {"Wall thickness", "16"},
  {"Textures", "world-aligned; 1 unit = 1 texel at scale 1"},
};

constexpr auto TargetingText = R"(# Targeting

## Node paths

`layer:<n>/group:<n>/entity:<n>/brush:<n>/face:<n>`: each index counts the preceding
siblings of the same kind in file order, so `layer:0/brush:1` is the default layer's
second brush even if an entity sits between the brushes. `layer:0` is the default layer
and `world` is worldspawn. Layers and groups can also be named by their persistent id,
as in `layer:id:<id>` or `group:id:<id>`, and a path can start with `group:id:<id>`.
Paths are valid only for the rev they came from.

## Targets

An op's `target` is one of:

- a node path: `"layer:0/brush:3"`;
- a binding: `"$floor"`, what an earlier op in the same batch created with `"as":
  "$floor"`; some ops bind a list of nodes;
- a list of paths and bindings: `["layer:0/brush:3", "$floor"]`;
- a query: `{"query": "<ql>", "expect": <n | "any">}`; `expect` defaults to `"any"`.

Targets are resolved against the input rev before the first op runs, so created nodes
are reachable only through bindings, and an op that targets a node an earlier op
removed fails with `target_deleted`. A binding is also accepted where an op takes one
node, such as `parent`.

On the command line, `--target` takes one path, and `--query '<ql>' --expect <n>` a
query. An op's other node arguments, such as `subtract`'s `cutters` and `from`,
take one path as a flag; use a batch for lists and queries.

## Node kinds

Each op accepts some node kinds, listed in `tbcli <op> --help` and as `x-node-kinds` in
`tbcli ops --json`. The kinds are {kinds}. Another kind fails with `wrong_node_kind`.
Where an argument has `x-expands-to-brushes`, a layer, group or brush entity stands for
the brushes inside it; transforms instead move groups and entities whole. Hidden state
is ignored, and locked nodes are refused unless `--allow-locked`.

## Faces

Face ops take face paths, or brush targets plus `faces`: `"all"` (the default), one of
{directions}, or a list of them. A direction picks the faces whose normal is within
about 0.5 degrees of that axis.

## Queries

TbQl selects nodes or faces, for example:

- `classname is "light"`: the entities of a class
- `properties.target == "door1"`: entities by a property
- `type is brush && entity.classname is "func_door"`: the brushes of doors
- `material == "sky1"`: the faces with a material, as face paths

Quote string values; a query that does not parse is silently run as a text search.
`tbcli query <map> '<ql>'` returns the matches in file order, at most `--limit` (200)
from `--offset`, with `total` and `truncated`; `--count-only` returns only the count.

## Spatial filters

`query` also takes `--within <box>` (bounds inside the box), `--intersects <box>`
(bounds overlapping it) and `--touching <path>` (bounds overlapping or touching that
node's bounds, leaving out the node, what contains it and what it contains). A box is
`x1,y1,z1,x2,y2,z2`. The filters compare bounding boxes, not shapes, with a tolerance
of 0.01 units, so boxes that only touch intersect.
)";

constexpr auto VerifyText = R"(# Verify

`tbcli verify <map>` checks the map in stages, in this order:

1. `load_diagnostics`: what the reader skipped, as `load_error` issues with a `line`.
2. `validators`: TrenchBroom's own checks.
3. `cli_checks`: tbcli's checks. `missing_material` is checked only when a Quake
   directory is configured; otherwise the stage lists it under `unchecked`.
4. With `--compile`: `qbsp`; with `--compile=vis` also `vis`, with `--compile=light`
   also `vis` and `light`.

Each stage has a `status`: `ok`, `issues`, `skipped` (with a `reason`) or `failed`.

## Reading the result

- `ok` is true when no unsuppressed issue is at or above the threshold: `error`, or the
  severity given to `--fail-on`.
- `counts` per issue type (plus `suppressed`) and `by_severity` always cover the whole
  map.
- `issues` are in file order, paged with `--limit` (200) and `--offset`, and narrowed
  with `--type`, `--min-severity` and `--target <path | ql>`.
- An issue has `type`, `severity`, `key`, `message`, `node` (a path, or null for an
  issue of the whole map), `line`, `origin` or `bounds`, and `fixes`.
- A `key` names the same issue across revs as long as its node is unchanged. Track
  and suppress issues by key, never by path.
- `--fail-on <severity>` exits 4 when an unsuppressed issue is at or above it.

The issue types, their default severities and their fix ids are in `tbcli ops --json`
under `issue_types`.

## Fixing issues

- Edit the node, or apply a quick-fix: `{"op": "fix", "issue_type": "<type>", "fix":
  "<fix id>"}` fixes every issue of that type; a `target` limits it to the issues on or
  inside the target.
- After every op, `issues_introduced` lists the issues it added on the nodes it created
  or changed. Checks of the whole map, such as `no_player_start`, and compiling run
  only in `verify`.

## Configuration

In tbcli.json, `verify.severity` changes the severity of a type, as in
`{"missing_definition": "error"}`, and `verify.ignore` suppresses issues, as in
`[{"type": "<type>", "key": "<key>"}]` or `[{"type": "<type>", "query": "<ql>"}]`.
Suppressed issues are counted, but leave `issues`, `ok` and `--fail-on`.

## Compiling

`--compile` runs qbsp on exactly the verified bytes in a temporary directory. The
compilers come from tbcli.json's `compilers` (`{"qbsp": "<path>"}` or `{"qbsp":
{"path": "<path>", "timeout": <seconds>}}`, likewise `vis` and `light`) or from
`--qbsp`, `--vis` and `--light`; a missing one fails with `compiler_missing`. `--timeout
<seconds>` overrides every tool's timeout, and `--artifacts <dir>` keeps the `.bsp`,
`.prt`, `.pts` and logs.

- Each warning or error in a compiler's log becomes a `compiler` issue with its `tool`,
  mapped to a node where the log names a line or an entity.
- A tool that fails or times out is a `compile_fatal` issue, not a failed command.
- vis and light are `skipped` with the reason `leak` when qbsp leaks or fails.
- Without a wad (worldspawn's `wad` key), qbsp warns about every texture.

## Leaks

A leak is one `leak` issue. Its `node` is the entity from which qbsp reached the void,
`trail` has the `start`, `end` and `turning_points` of the way out, and
`nearest_brushes` are the world, `func_group` and `func_detail` brushes nearest to where
the trail leaves the map. The hole is there: close it with a world brush, then run
`verify --compile` again. Only world and `func_group` brushes reliably seal: qbsp lets
the void through `func_detail`, `func_wall` and other brush entities. Every point
entity must be inside the sealed space.

## Rooms

`tbcli rooms <map>` reports a graph of the map's spaces, always marked `heuristic`:
rooms with their bounds, contents and entities, and the openings between them with
their centre, size, the `brushes` framing them and `gated_by`. It runs qbsp itself
(with `-nodetail -forceprt1`). A leaking map fails with `map_leaks`, which carries the
`leak` issue; fix the leak first. A failed qbsp fails with `compile_failed`.

- `summary.components` above 1 means some rooms cannot be reached from others;
  teleporters count as connections.
- `gated_by` lists every brush entity that overlaps the opening, triggers included;
  check the classname to find doors.
- An opening at a water, slime or lava surface has `kind: "liquid_surface"`; other
  openings have no kind.
- `--split-ratio` (default 0.5) tunes where rooms are cut, and `--cells` lists the
  unmerged cells.
- `--from-artifacts <dir>` reuses the artifacts of `verify --compile --artifacts <dir>`
  for the same rev without a leak, but only for maps without detail brushes or liquids:
  for those, verify's `.prt` is clustered, and rooms fails with `stale_artifact`.
)";

constexpr auto GenerateText = R"(# Generate

This topic is not written yet. Until it is: lay out rooms and the corridors between
them with `add_room` and `connect`, change them with `edit_room` and `edit_connection`
rather than by editing their brushes, and read `tbcli <op> --help` for their arguments.
)";

constexpr auto ExamplesText = R"(# Examples

There are no worked examples yet. Follow the loop in `tbcli guide`, and build batches
from `tbcli ops --json`.
)";

constexpr auto SkillText = R"(---
name: tbcli
description: "Reading, editing, generating or verifying Quake `.map` files with tbcli. Use for any task that inspects or changes a .map file, lays out rooms, or checks that a map compiles and is sealed."
---

# tbcli

Before you work on a Quake `.map` file, run `tbcli guide` and follow it. The guide is
built into tbcli, so it always matches the installed version. It lists topics that
`tbcli guide <topic>` loads on demand.
)";

constexpr auto AgentsSnippetText = R"(## Quake maps

To read, edit, generate or verify Quake `.map` files, use `tbcli`. Run `tbcli guide`
first and follow it.
)";

constexpr auto WrapWidth = size_t(88);

/**
 * Breaks the lines of Markdown text that are longer than WrapWidth at spaces, indenting
 * the continuation lines of list items. Code blocks and table rows are left as they
 * are. Only lines with generated content are long, since the guide's own text is
 * wrapped.
 */
std::string wrapLongLines(const std::string_view text)
{
  auto result = std::string{};
  auto inCodeBlock = false;
  for (const auto line : text | std::views::split('\n'))
  {
    auto rest = std::string{line.begin(), line.end()};
    if (rest.starts_with("```"))
    {
      inCodeBlock = !inCodeBlock;
    }
    if (!inCodeBlock && !rest.starts_with("|"))
    {
      const auto content = rest.find_first_not_of(' ');
      const auto isListItem =
        content != std::string::npos && rest.compare(content, 2, "- ") == 0;
      const auto indent = std::string(
        content == std::string::npos ? 0 : content + (isListItem ? 2 : 0), ' ');
      while (rest.size() > WrapWidth)
      {
        const auto space = rest.rfind(' ', WrapWidth);
        if (space == std::string::npos || space <= indent.size())
        {
          break;
        }
        result += rest.substr(0, space) + "\n";
        rest = indent + rest.substr(space + 1);
      }
    }
    result += rest + "\n";
  }
  // splitting yields an empty last line after the final line break
  result.pop_back();
  return result;
}

/**
 * The text with each placeholder, such as "{table}", replaced by its value. The values
 * are not searched for placeholders.
 */
std::string fillPlaceholders(
  const std::string_view text,
  const std::initializer_list<std::pair<std::string_view, std::string_view>> values)
{
  auto result = std::string{text};
  for (const auto& [placeholder, value] : values)
  {
    result = kdl::str_replace_every(result, placeholder, value);
  }
  return result;
}

/** The names in backticks, as Markdown code. */
std::vector<std::string> quoted(const std::ranges::input_range auto& names)
{
  auto result = std::vector<std::string>{};
  std::ranges::transform(names, std::back_inserter(result), [](const auto& name) {
    return fmt::format("`{}`", name);
  });
  return result;
}

/** The names as a list in prose, as in "`a`, `b` and `c`" for the conjunction "and". */
std::string proseList(
  const std::vector<std::string_view>& names, const std::string_view conjunction)
{
  const auto items = quoted(names);
  if (items.size() < 2)
  {
    return kdl::str_join(items, "");
  }
  return fmt::format(
    "{} {} {}",
    kdl::str_join(std::ranges::subrange(items.begin(), items.end() - 1), ", "),
    conjunction,
    items.back());
}

/** A Markdown table cell: pipes are escaped, line breaks become spaces. */
std::string tableCell(const std::string_view text)
{
  return kdl::str_replace_every(
    kdl::str_replace_every(std::string{text}, "|", "\\|"), "\n", " ");
}

std::string conventions(const GuideConfig& config)
{
  const auto& rows = config.conventions ? *config.conventions : DefaultConventions;

  auto table = std::string{"| Convention | Value |\n|---|---|\n"};
  for (const auto& [name, value] : rows)
  {
    table += fmt::format("| {} | {} |\n", tableCell(name), tableCell(value));
  }

  return fillPlaceholders(
    ConventionsText,
    {
      {"{source}",
       config.conventions ? ConfiguredConventionsSource : DefaultConventionsSource},
      {"{table}", table},
      {"{notes}", config.conventions ? "" : DefaultConventionsNotes},
    });
}

std::string targeting(const GuideConfig&)
{
  auto kinds = std::vector<std::string_view>{};
  for (const auto kind : {
         TargetKind::World,
         TargetKind::Layer,
         TargetKind::Group,
         TargetKind::PointEntity,
         TargetKind::BrushEntity,
         TargetKind::Brush,
         TargetKind::Patch,
         TargetKind::Face,
       })
  {
    kinds.push_back(toString(kind));
  }

  const auto directions = faceDirectionNames();
  return fillPlaceholders(
    TargetingText,
    {
      {"{kinds}", proseList(kinds, "and")},
      {"{directions}", proseList({directions.begin(), directions.end()}, "or")},
    });
}

struct GuideTopic
{
  std::string_view name;
  /** One line for the topic index. */
  std::string_view summary;
  std::string (*text)(const GuideConfig&);
  /** Whether the text depends on tbcli.json. */
  bool readsConfig = false;
};

constexpr auto Topics = std::array<GuideTopic, 5>{{
  {"conventions",
   "player size, step and jump heights, corridor and door sizes, grid, wall "
   "thickness and texture scale",
   conventions,
   true},
  {"targeting",
   "node paths, bindings, lists, queries with `expect`, node kinds, face selectors "
   "and spatial filters",
   targeting},
  {"verify",
   "stages, issues, keys, suppression, quick-fixes, compiling, leaks and the rooms "
   "graph",
   [](const GuideConfig&) { return std::string{VerifyText}; }},
  {"generate",
   "laying out rooms and corridors as built rooms",
   [](const GuideConfig&) { return std::string{GenerateText}; }},
  {"examples",
   "worked batches with their results",
   [](const GuideConfig&) { return std::string{ExamplesText}; }},
}};

constexpr auto TopicNames = []() {
  auto result = std::array<std::string_view, Topics.size()>{};
  std::ranges::transform(Topics, result.begin(), &GuideTopic::name);
  return result;
}();

const GuideTopic* findTopic(const std::string_view name)
{
  const auto found = std::ranges::find(Topics, name, &GuideTopic::name);
  return found != Topics.end() ? &*found : nullptr;
}

std::string topicIndex()
{
  auto result = std::string{};
  for (const auto& topic : Topics)
  {
    result += fmt::format("- `{}`: {}\n", topic.name, topic.summary);
  }
  return result;
}

std::string opIndex()
{
  return kdl::str_join(quoted(opDefs() | std::views::transform(&OpDef::name)), ", ")
         + "\n";
}

std::string overview()
{
  return fillPlaceholders(
    OverviewText, {{"{topics}", topicIndex()}, {"{ops}", opIndex()}});
}

} // namespace

std::span<const std::string_view> guideTopics()
{
  return TopicNames;
}

bool guideTopicReadsConfig(const std::string_view topic)
{
  const auto* found = findTopic(topic);
  return found && found->readsConfig;
}

std::string guideText(
  const std::optional<std::string_view> topic, const GuideConfig& config)
{
  if (!topic)
  {
    return wrapLongLines(overview());
  }

  const auto* found = findTopic(*topic);
  if (!found)
  {
    throw usageError(
      fmt::format(
        "unknown guide topic '{}', expected {}",
        *topic,
        kdl::str_join(guideTopics(), ", ")));
  }
  return wrapLongLines(found->text(config));
}

Json installGuide(const std::filesystem::path& dir)
{
  auto ec = std::error_code{};
  std::filesystem::create_directories(dir, ec);
  if (ec)
  {
    throw CliError{
      ErrorCode::WriteFailed,
      fmt::format("cannot create directory {}: {}", dir, ec.message())};
  }

  const auto write = [&](const std::string_view fileName, const std::string_view text) {
    const auto path = (dir / fileName).lexically_normal();
    if (const auto error = writeFile(path, text))
    {
      throw CliError{ErrorCode::WriteFailed, *error};
    }
    return fmt::format("{}", path);
  };

  auto result = Json::object();
  result.set("schema", "tbcli.guide_install/1");
  result.set("skill", write(GuideSkillFileName, SkillText));
  result.set("agents_snippet", write(GuideAgentsSnippetFileName, AgentsSnippetText));
  return result;
}

} // namespace tb::cli
