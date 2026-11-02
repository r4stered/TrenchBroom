#!/usr/bin/env bash
#
# Re-records the compiled outputs of the maps in this directory with the pinned
# ericw-tools version. See scripts/record-ericw-tools.sh. The tbcli compiler tests
# replay these recordings through a fake compiler.
#
#   compile_warnings.map  two sealed rooms with a brush that mixes water and wall faces
#                         (a warning qbsp ties to a line of the map) and an info_null
#                         with a long property key (a warning qbsp ties to the
#                         entity's classname and origin)
#   compile_fatal.map     the same rooms with a func_wall made of region faces, which
#                         qbsp refuses with an error and a non-zero exit code
#   compile_leak.map      two rooms joined by a doorway (TbMdlLib's rooms_prt1.map)
#                         with a hole where the east wall stops short of the north
#                         wall, so qbsp writes a pointfile from the light to the void;
#                         recorded with -keepprt, as tbcli runs qbsp
#   compile_full.map      the sealed rooms_prt1.map with a light of an unknown delay (a
#                         warning light ties to the entity's classname and origin),
#                         recorded through qbsp, vis -fast and light
#   rooms_prt1, rooms_prt2_water
#                         TbMdlLib's CompiledMaps fixtures, which the test build copies
#                         here; recorded with -nodetail -forceprt1, as tbcli rooms runs
#                         qbsp, so the water map's .prt has one cell per leaf, too
#   rooms_slot.map        rooms_prt1.map with an 8 unit wide, 80 unit deep slot in room
#                         A's west wall, recorded as tbcli rooms runs qbsp
#   rooms_sky.map         rooms_prt1.map with a sky ceiling, recorded as tbcli rooms runs
#                         qbsp
#   rooms_openings.map    rooms_prt1.map whose dividing wall has an L-shaped gap (a
#                         doorway, closed by a func_door, beside a window) and a
#                         separate window, and a sealed third room that a
#                         trigger_teleport in room A leads to; recorded as tbcli rooms
#                         runs qbsp
#   rooms_prt2_water_keepprt
#                         rooms_prt2_water.map recorded as verify runs qbsp, which
#                         writes a PRT2 file of clusters
#   built_rooms.map       two rooms laid out by tbcli add_room (A at the origin, B east
#                         of it) with a player start in A and a light in B, without the
#                         Generator line; tst_BuiltRooms regenerates it and compares;
#                         recorded with -keepprt, as verify runs qbsp
#
# Usage: ERICW_TOOLS_DIR=<dir> lib/TbCliLib/test/fixture/compiled/record.sh

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
record="$(git -C "$here" rev-parse --show-toplevel)/scripts/record-ericw-tools.sh"

"$record" "$here/compile_warnings.map" "$here/compile_warnings"
# qbsp fails on purpose, so the script's exit code is ignored
"$record" "$here/compile_fatal.map" "$here/compile_fatal" || true
"$record" --qbsp-args "-keepprt" "$here/compile_leak.map" "$here/compile_leak"
"$record" --vis "-fast" --light "" "$here/compile_full.map" "$here/compile_full"

mdl="$(git -C "$here" rev-parse --show-toplevel)/lib/TbMdlLib/test/fixture/mdl/CompiledMaps"
"$record" --qbsp-args "-nodetail -forceprt1" "$mdl/rooms_prt1.map" "$here/rooms_prt1"
"$record" --qbsp-args "-nodetail -forceprt1" "$mdl/rooms_prt2_water.map" "$here/rooms_prt2_water"
"$record" --qbsp-args "-nodetail -forceprt1" "$here/rooms_slot.map" "$here/rooms_slot"
"$record" --qbsp-args "-nodetail -forceprt1" "$here/rooms_sky.map" "$here/rooms_sky"
"$record" --qbsp-args "-nodetail -forceprt1" "$here/rooms_openings.map" "$here/rooms_openings"
"$record" --qbsp-args "-keepprt" "$here/built_rooms.map" "$here/built_rooms"
# recorded under the name that the test build copies the map to
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$mdl/rooms_prt2_water.map" "$tmp/rooms_prt2_water_keepprt.map"
"$record" --qbsp-args "-keepprt" "$tmp/rooms_prt2_water_keepprt.map" "$here/rooms_prt2_water_keepprt"
