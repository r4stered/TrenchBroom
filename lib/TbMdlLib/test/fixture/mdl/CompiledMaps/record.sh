#!/usr/bin/env bash
#
# Re-records the compiled outputs of the maps in this directory with the pinned
# ericw-tools version. See scripts/record-ericw-tools.sh.
#
#   rooms_prt1.map        two rooms joined by a doorway; no detail or liquids, so PRT1
#   rooms_prt2_water.map  the same rooms with a water pool and a func_detail pillar;
#                         qbsp treats both as detail, groups vis leafs into
#                         clusters and writes PRT2 (water alone gives PRT2, too)
#
# rooms_prt1 is recorded twice, as BSP29 (the default) and as BSP2 (-bsp2).
#
# Usage: ERICW_TOOLS_DIR=<dir> lib/TbMdlLib/test/fixture/mdl/CompiledMaps/record.sh

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
record="$(git -C "$here" rev-parse --show-toplevel)/scripts/record-ericw-tools.sh"

"$record" "$here/rooms_prt1.map" "$here/rooms_prt1"
"$record" --qbsp-args "-bsp2" "$here/rooms_prt1.map" "$here/rooms_prt1_bsp2"
"$record" "$here/rooms_prt2_water.map" "$here/rooms_prt2_water"
