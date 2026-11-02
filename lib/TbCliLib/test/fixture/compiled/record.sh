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
