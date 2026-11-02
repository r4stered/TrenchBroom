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

#include "cli/BuiltRooms.h"
#include "cli/CliError.h"
#include "cli/FaceOps.h"
#include "cli/FileUtils.h"
#include "cli/Ops.h"
#include "cli/Targets.h"

#include "kd/contracts.h"
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

Lay out new rooms and the corridors between them as built rooms, not as loose brushes.
`add_room` builds a sealed room and `connect` a straight corridor between two rooms.
Both are ordinary brushes in a `func_group` that keeps their parameters, so that tbcli
can rebuild them when the parameters change. Read `tbcli <op> --help` for the arguments.

## Lay out rooms and connections first

- Lay out every room and connection before you add detail.
- Place the first room with `min`, the minimum corner of its interior, and every other room with `place` next to a room that is built already: `{"<side>": "<room>", "gap": <n>}`, where the side is {relations}. The room then stands `gap` units from the other room's interior, centred on it along the other horizontal axis, with its floor at the same height.
- `size` is the interior, the space a player moves through. Walls, floor and ceiling `wall` units thick ({wall}) go around it. Size rooms and corridors by `tbcli guide conventions`.
- `connect` joins two rooms that stand side by side along x or y with their floors at the same height. The corridor's interior is `width` by `height` ({width} by {height}), on the rooms' floor, centred on the wall that they share and moved along it by `offset`. Its length is the gap minus both rooms' walls, so leave a gap larger than the two walls together. Two rooms have at most one connection.
- Rooms are named with letters, digits and underscores; a connection is named by its
  rooms, as in `A-B`.
- Put entities, such as the player start and lights, inside the rooms.
- Built faces have no material. Retexture them with `set_material`, for example with
  the query `material == "__TB_empty"`.

## Edit rooms through their parameters

- Change a room with `edit_room` (`size`, `wall`, `place`) and a connection with
  `edit_connection` (`width`, `height`, `offset`). tbcli rebuilds it and what depends on
  it: rooms placed against it are placed again, and its connections are rebuilt and cut
  into the walls again. Rebuilt brushes keep the materials of the brushes they replace.
- Never edit the brushes of a room or connection: don't move, cut, clip, merge or add to
  them. tbcli then refuses to rebuild or connect the room with `hand_edited`. Moving a
  room's whole group and retexturing are fine.
- `tbcli describe <map>` lists the built rooms under `built_rooms`, each with its `params` and a `status` of {statuses}. Connections are the groups named `connection:<A>-<B>`.
- `force` rebuilds a hand-edited room or connection anyway and loses the hand edits.
  `detach` turns one into plain brushes that tbcli no longer rebuilds.
- A room that no longer stands where its placement puts it stays where it is, with the
  warning `stale_placement`, and is listed under `stale_placements`.
- Rooms in linked groups are one room with several instances. Edit it once and every
  instance follows; `instances_affected` lists the linked copies that the edit updated.

## Add detail with box ops

- Add pillars, steps, platforms and trim with `create_box`, `create_brush` and
  `create_shape`, inside the rooms or between them. They are brushes of their own,
  outside the rooms, so the rooms stay rebuildable.
- Rebuilding a room does not move its detail; move that with `translate`.
- For an opening that `connect` cannot build, such as a window, `detach` the room and
  cut it with `subtract`, as in the doorway in `tbcli guide examples`. The room is then
  plain brushes for good, and a room that a connection joins to it cannot be rebuilt
  until that connection is detached too.

## Check every batch

- Run `tbcli verify <map> --compile` after every batch. Done when `ok` is true and
  there is no `leak` issue.
- A refusal from `add_room`, `connect`, `edit_room` or `edit_connection`, such as {layout_errors}, is a layout error: change the sizes, gaps, offsets or placements until the layout fits. Don't work around it with boxes, which tbcli cannot rebuild and which easily leak.

{examples})";

constexpr auto ExamplesText = R"(# Examples

Worked batches with the results tbcli returns. Each runs on the map it describes; the
paths and coordinates are that map's. `<rev>` is the map's rev before the batch and
`<new rev>` the rev the batch writes. Rehearse each batch with `--dry-run` first, as the
loop in `tbcli guide` says.

{examples})";

/** The guide's examples, initialized on first use, not at static initialization. */
const std::vector<GuideExample>& allExamples()
{
  static const auto examples = std::vector<GuideExample>{
    {
      "examples",
      "retexture",
      "1. Inspect, then retexture by query",
      R"(The map has two rooms side by side along x, both from y -128 to 128, with their
floors at z 0 and their ceilings at z 128: the west room, with the player start, from x
-256 to 0, and the east room, with a light, from x 16 to 272. A doorway in the wall
between them joins them. Change the floor's material: first see which materials the map
uses, then count the faces that have the floor's.)",
      {
        {
          "tbcli describe <map>",
          R"({"textures": {"items": [{"name": "wall1_1", "count": 42},
                       {"name": "ceiling1_1", "count": 6},
                       {"name": "ground1_6", "count": 6}]}})",
          true,
        },
        {
          R"(tbcli query <map> 'material == "ground1_6"' --count-only)",
          R"({"schema": "tbcli.query/1", "rev": "<rev>", "total": 6})",
        },
      },
      R"(Set the material on the faces the query matches. `expect` makes the batch fail with
`expect_mismatch`, writing nothing, if the query matches a different number of faces.)",
      R"([{"op": "set_material",
  "target": {"query": "material == \"ground1_6\"", "expect": 6},
  "material": "ground1_2"}])",
      R"({"schema": "tbcli.apply/1", "rev": "<new rev>", "ops": [
  {"ok": true, "created": [], "modified": ["layer:0/brush:0"], "removed": [],
   "warnings": [], "issues_introduced": []}],
 "bindings": {}})",
      R"(`modified` names the floor brush, whose faces changed, as a path in the new rev.)",
      {
        {
          R"(tbcli query <map> 'material == "ground1_2"' --count-only)",
          R"({"schema": "tbcli.query/1", "rev": "<new rev>", "total": 6})",
        },
      },
      {},
    },
    {
      "examples",
      "doorway",
      "2. Add a sealed room joined to a room by a doorway",
      R"(On the map of example 1, add a room east of the east room, built from plain
brushes, and join the two by a doorway. For rooms that tbcli can rebuild, see `tbcli
guide generate`. Find the map's extent, then the wall to build against.)",
      {
        {
          "tbcli describe <map>",
          R"({"bounds": {"min": [-272, -144, -16], "max": [288, 144, 144]}})",
          true,
        },
        {
          "tbcli query <map> 'type is brush' --within 256,-256,-256,512,256,256",
          R"({"schema": "tbcli.query/1", "rev": "<rev>", "items": [
  {"path": "layer:0/brush:3", "aabb": {"min": [272, -144, 0], "max": [288, 144, 128]},
   "axial_box": true, "textures": ["wall1_1"], "entity": "world"}],
 "total": 1, "truncated": false})",
        },
      },
      R"(The east wall is 16 units thick, from x 272 to 288. The batch makes a box for the
outside of the new room against it, from the floor's bottom to the ceiling's top, and
hollows it into walls 16 units thick. It cuts a doorway 64 wide and 96 tall through
both walls with a box that `subtract` takes away from the brushes of `layer:0` that it
overlaps, and puts a light in the new room. The layer stands for the brushes that are
in it when `subtract` runs, so the new walls are cut too.)",
      R"([{"op": "create_box", "min": [288, -144, -16], "max": [560, 144, 144],
  "material": "wall1_1", "as": "$room"},
 {"op": "hollow", "target": "$room", "thickness": 16},
 {"op": "create_box", "min": [272, -32, 0], "max": [304, 32, 96],
  "material": "wall1_1", "as": "$door"},
 {"op": "subtract", "cutters": "$door", "from": "layer:0"},
 {"op": "create_point_entity", "classname": "light", "origin": [416, 0, 96]}])",
      R"({"schema": "tbcli.apply/1", "rev": "<new rev>", "ops": [
  {"ok": true, "created": [], "modified": [], "removed": [], "warnings": [],
   "issues_introduced": []},
  {"ok": true,
   "created": ["layer:0/brush:8", "layer:0/brush:9", "layer:0/brush:10",
               "layer:0/brush:11", "layer:0/brush:12"],
   "modified": [], "removed": [], "warnings": [], "issues_introduced": []},
  {"ok": true, "created": [], "modified": [], "removed": [], "warnings": [],
   "issues_introduced": []},
  {"ok": true,
   "created": ["layer:0/brush:13", "layer:0/brush:14", "layer:0/brush:15",
               "layer:0/brush:16", "layer:0/brush:17", "layer:0/brush:18",
               "layer:0/brush:19"],
   "modified": [], "removed": ["layer:0/brush:3"], "warnings": [],
   "issues_introduced": []},
  {"ok": true, "created": ["layer:0/entity:2"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []}],
 "bindings": {}})",
      R"(`created` lists only what is still there after the batch: not the box that `hollow`
replaced, the door, or the new west wall that `subtract` cut. The bindings are gone with
the boxes. `removed` lists the east wall that `subtract` cut, as a path in the old rev.)",
      {
        {
          "tbcli verify <map> --compile",
          R"({"rev": "<new rev>", "ok": true, "by_severity": {"error": 0}})",
          true,
        },
      },
      R"(`ok` is true and there is no `leak` issue: the new room is sealed. qbsp's warnings
about textures it cannot find are `compiler` issues of severity warning.)",
    },
    {
      "examples",
      "leak",
      "3. Fix a leak",
      R"(The map is that of example 1, except that the east wall of the east room stops short
of the north wall. Compile the map and read the leak.)",
      {
        {
          "tbcli verify <map> --compile --type leak",
          R"({"ok": false, "issues": {"items": [
  {"type": "leak", "severity": "error", "node": "layer:0/entity:1",
   "origin": [144, 0, 96],
   "trail": {"start": [144, 0, 96], "end": [-272, 156, -20],
             "turning_points": [[144, 32, 64], [272, 96, 64], [280, 156, 0]]},
   "nearest_brushes": ["layer:0/brush:3", "layer:0/brush:5", "layer:0/brush:0"]}]}})",
          true,
        },
        {
          "tbcli query <map> 'type is brush' --intersects 264,48,8,296,160,120",
          R"({"schema": "tbcli.query/1", "rev": "<rev>", "items": [
  {"path": "layer:0/brush:3", "aabb": {"min": [272, -144, 0], "max": [288, 64, 128]},
   "axial_box": true, "textures": ["wall1_1"], "entity": "world"},
  {"path": "layer:0/brush:5", "aabb": {"min": [-256, 128, 0], "max": [272, 144, 128]},
   "axial_box": true, "textures": ["wall1_1"], "entity": "world"}],
 "total": 2, "truncated": false})",
        },
      },
      R"(The way out runs from the light past (272, 96) and leaves the map at (280, 156). The
query looks at the walls around those points, the first two of `nearest_brushes`: the
east wall ends at y 64, and the north wall at x 272. The hole is the corner between
them, from x 272 to 288 and from y 64 to 144. Close it with a world brush.)",
      R"([{"op": "create_box", "min": [272, 64, 0], "max": [288, 144, 128],
  "material": "wall1_1"}])",
      R"({"schema": "tbcli.apply/1", "rev": "<new rev>", "ops": [
  {"ok": true, "created": ["layer:0/brush:9"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []}],
 "bindings": {}})",
      R"(`issues_introduced` is empty, but only `verify --compile` tells whether the map is
sealed now.)",
      {
        {
          "tbcli verify <map> --compile",
          R"({"rev": "<new rev>", "ok": true, "by_severity": {"error": 0}})",
          true,
        },
      },
      R"(`ok` is true and there is no `leak` issue. If the map still leaked, the new `leak`
issue would show the next hole.)",
    },
    {
      "generate",
      "rooms",
      "Example: two rooms and a corridor",
      R"(On an empty map, lay out room A at the origin and room B east of it, put the player
start in A and a light in B, and join the rooms.)",
      {},
      R"(B is placed 128 units east of A's interior. Both walls are 16 units thick, so the
corridor is 96 units long. It is 96 units tall, since B is only 96 units tall.)",
      R"([{"op": "add_room", "name": "A", "min": [0, 0, 0], "size": [256, 256, 128]},
 {"op": "add_room", "name": "B", "size": [192, 320, 96],
  "place": {"east_of": "A", "gap": 128}},
 {"op": "create_point_entity", "classname": "info_player_start",
  "origin": [128, 128, 24]},
 {"op": "create_point_entity", "classname": "light", "origin": [480, 128, 64]},
 {"op": "connect", "from": "A", "to": "B", "height": 96}])",
      R"({"schema": "tbcli.apply/1", "rev": "<new rev>", "ops": [
  {"ok": true, "created": ["layer:0/group:0"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []},
  {"ok": true, "created": ["layer:0/group:1"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []},
  {"ok": true, "created": ["layer:0/entity:0"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []},
  {"ok": true, "created": ["layer:0/entity:1"], "modified": [], "removed": [],
   "warnings": [], "issues_introduced": []},
  {"ok": true, "created": ["layer:0/group:2"],
   "modified": ["layer:0/group:0/entity:0", "layer:0/group:1/entity:0"],
   "removed": [], "warnings": [], "issues_introduced": []}],
 "bindings": {}})",
      R"(Each room is a group `built_room:<name>` around its carrier, and the corridor is the
group `connection:A-B`. `connect` modified both rooms' carriers, whose walls it cut.)",
      {
        {
          "tbcli describe <map>",
          R"({"built_rooms": {"items": [
  {"name": "A", "params": {"size": [256, 256, 128], "wall": 16},
   "status": "rebuildable", "group": "layer:0/group:0"},
  {"name": "B",
   "params": {"size": [192, 320, 96], "wall": 16,
              "place": {"east_of": "A", "gap": 128}},
   "status": "rebuildable", "group": "layer:0/group:1"}],
 "total": 2}})",
          true,
        },
        {
          "tbcli verify <map> --compile",
          R"({"rev": "<new rev>", "ok": true, "by_severity": {"error": 0}})",
          true,
        },
      },
      R"(To make B wider, `edit_room` B with a new `size`: tbcli rebuilds B and the corridor,
and cuts A's wall again.)",
    },
  };
  return examples;
}

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

std::string renderCommand(const GuideCommand& command)
{
  return fmt::format(
    "`{}` returns{}:\n\n```json\n{}\n```\n",
    command.command,
    command.excerpt ? ", among other fields" : "",
    command.output);
}

std::string renderExample(const GuideExample& example)
{
  auto result = fmt::format("## {}\n\n{}\n", example.title, example.intro);
  for (const auto& command : example.inspect)
  {
    result += "\n" + renderCommand(command);
  }
  result += fmt::format(
    "\n{}\n\n```json\n{}\n```\n\n"
    "`tbcli apply <map> batch.json --expect-rev <rev>` returns:\n\n```json\n{}\n```\n\n"
    "{}\n",
    example.plan,
    example.batch,
    example.result,
    example.review);
  for (const auto& command : example.check)
  {
    result += "\n" + renderCommand(command);
  }
  if (!example.outro.empty())
  {
    result += fmt::format("\n{}\n", example.outro);
  }
  return result;
}

/** The examples that the given topic shows, as Markdown sections. */
std::string renderExamples(const std::string_view topic)
{
  auto sections = std::vector<std::string>{};
  for (const auto& example : allExamples())
  {
    if (example.topic == topic)
    {
      sections.push_back(renderExample(example));
    }
  }
  return kdl::str_join(sections, "\n");
}

/** The default value of an op's argument, as JSON. */
std::string defaultValueOf(const std::string_view opName, const std::string_view argName)
{
  const auto* def = findOpDef(opName);
  contract_assert(def != nullptr);
  const auto arg = std::ranges::find(def->args, argName, &ArgDef::name);
  contract_assert(arg != def->args.end() && arg->defaultValue);
  return arg->defaultValue->dump();
}

std::string generate(const GuideConfig&)
{
  const auto relations = relationNames();
  return fillPlaceholders(
    GenerateText,
    {
      {"{wall}", defaultValueOf("add_room", "wall")},
      {"{width}", defaultValueOf("connect", "width")},
      {"{height}", defaultValueOf("connect", "height")},
      {"{relations}", proseList({relations.begin(), relations.end()}, "or")},
      {"{statuses}",
       proseList(
         {toString(RoomStatus::Rebuildable), toString(RoomStatus::HandEdited)}, "or")},
      {"{layout_errors}",
       proseList(
         {
           toString(ErrorCode::LayoutOverlap),
           toString(ErrorCode::LayoutCorridorDoesNotFit),
           toString(ErrorCode::LayoutFloorMismatch),
           toString(ErrorCode::LayoutCorridorTooTall),
         },
         "or")},
      // last, since the examples' text is not searched for placeholders
      {"{examples}", renderExamples("generate")},
    });
}

std::string examples(const GuideConfig&)
{
  return fillPlaceholders(ExamplesText, {{"{examples}", renderExamples("examples")}});
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
  {"generate", "laying out rooms and corridors as built rooms", generate},
  {"examples", "worked batches with their results", examples},
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

std::span<const GuideExample> guideExamples()
{
  return allExamples();
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
