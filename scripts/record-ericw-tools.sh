#!/usr/bin/env bash
#
# Records the outputs of the ericw-tools compilers for a .map file so that tests can
# use them without running the compilers.
#
# The recorded outputs are checked in as test fixtures: the TbMdlLib parser tests read
# the .bsp and .prt files, and the tbcli compiler tests replay all of them through a
# fake compiler. Unit tests never run the real compilers.
#
# Usage:
#   scripts/record-ericw-tools.sh [options] <map> <out-dir>
#
# Options:
#   --tools-dir <dir>    Directory containing qbsp, vis and light, or their build
#                        subdirectories qbsp/, vis/ and light/ (default: $ERICW_TOOLS_DIR).
#   --qbsp-args "<args>" Extra arguments for qbsp (default: none).
#   --vis "<args>"       Also run vis with these arguments (use "" for none).
#   --light "<args>"     Also run light with these arguments (use "" for none).
#   --any-version        Do not check the ericw-tools version.
#
# The map is copied into a temporary working directory, then qbsp, vis and light run in
# that order. vis and light run only if requested and only if the previous tool exited
# with 0. If a tool fails, the script exits with its exit code after recording it. After each tool, the files it created or changed are copied to
# <out-dir>/<tool>/, together with a file named exit_code that holds its exit code.
# Only these files are recorded:
#
#   <name>.bsp, <name>.prt, <name>.pts, <name>.lit   compiler outputs
#   <name>.log, <name>-vis.log, <name>-light.log     the logs of qbsp, vis and light
#
# The logs contain absolute paths of the temporary working directory. Each such path
# prefix is replaced by the placeholder ${WORKDIR}, so that the recordings do not depend
# on the machine they were made on, and a replaying fake compiler can substitute its
# own working directory.
#
# The recordings are pinned to one ericw-tools version, ERICW_TOOLS_VERSION below
# (https://github.com/ericwa/ericw-tools, commit 36eec1da). Recording with another
# version is refused unless --any-version is given. When changing the pinned version,
# re-record every fixture that was made with this script; the fixture directories each
# contain a record.sh that does that.

set -euo pipefail

readonly ERICW_TOOLS_VERSION="2.0.0-alpha11-9-g36eec1da"

usage() {
  sed -n '/^# Usage:/,/^# The map is copied/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
  exit 2
}

tools_dir="${ERICW_TOOLS_DIR:-}"
qbsp_args=""
run_vis=0
vis_args=""
run_light=0
light_args=""
check_version=1
positional=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tools-dir) tools_dir="$2"; shift 2 ;;
    --qbsp-args) qbsp_args="$2"; shift 2 ;;
    --vis) run_vis=1; vis_args="$2"; shift 2 ;;
    --light) run_light=1; light_args="$2"; shift 2 ;;
    --any-version) check_version=0; shift ;;
    -h|--help) usage ;;
    -*) echo "unknown option: $1" >&2; usage ;;
    *) positional+=("$1"); shift ;;
  esac
done

[[ ${#positional[@]} -eq 2 ]] || usage
[[ -n "$tools_dir" ]] || { echo "no tools dir: pass --tools-dir or set ERICW_TOOLS_DIR" >&2; exit 2; }

map_file="${positional[0]}"
out_dir="${positional[1]}"
[[ -f "$map_file" ]] || { echo "map not found: $map_file" >&2; exit 2; }

find_tool() {
  local name="$1"
  local candidate
  for candidate in "$tools_dir/$name" "$tools_dir/$name.exe" "$tools_dir/$name/$name" "$tools_dir/$name/$name.exe"; do
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return
    fi
  done
  echo "$name not found in $tools_dir" >&2
  exit 2
}

qbsp="$(find_tool qbsp)"

if [[ $check_version -eq 1 ]]; then
  banner="$("$qbsp" 2>&1 | head -n 1 || true)"
  if [[ "$banner" != *"ericw-tools $ERICW_TOOLS_VERSION "* ]]; then
    echo "expected ericw-tools $ERICW_TOOLS_VERSION, but qbsp reports: $banner" >&2
    echo "pass --any-version to record anyway" >&2
    exit 1
  fi
fi

name="$(basename "$map_file" .map)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
cp "$map_file" "$work_dir/$name.map"

recorded_files=("$name.bsp" "$name.prt" "$name.pts" "$name.lit" "$name.log" "$name-vis.log" "$name-light.log")

checksums() {
  local file
  for file in "${recorded_files[@]}"; do
    if [[ -f "$work_dir/$file" ]]; then
      echo "$file $(md5sum < "$work_dir/$file")"
    fi
  done
}

# Paths of the working directory as the tools may print them: POSIX, Windows with
# backslashes and Windows with forward slashes.
work_dir_spellings=("$work_dir")
if command -v cygpath > /dev/null; then
  work_dir_spellings+=("$(cygpath -w "$work_dir")" "$(cygpath -m "$work_dir")")
fi

sanitize_log() {
  local file="$1"
  local spelling escaped
  for spelling in "${work_dir_spellings[@]}"; do
    escaped="$(printf '%s' "$spelling" | sed 's/[][\\/.*^$]/\\&/g')"
    # no sed -i, which differs between GNU and BSD sed
    sed "s/$escaped/\${WORKDIR}/g" "$file" > "$file.tmp"
    mv "$file.tmp" "$file"
  done
}

# Runs one tool in the working directory and records its outputs. Returns its exit code.
# The executable is looked up by name, so that vis and light need to exist only when
# they are run.
record() {
  local tool="$1"
  local args="$2"
  local executable before after file tool_dir exit_code

  executable="$(find_tool "$tool")"

  before="$(checksums)"

  set +e
  # shellcheck disable=SC2086 # args are deliberately split
  (cd "$work_dir" && "$executable" $args "$name.map" > /dev/null 2>&1)
  exit_code=$?
  set -e

  after="$(checksums)"

  tool_dir="$out_dir/$tool"
  rm -rf "$tool_dir"
  mkdir -p "$tool_dir"
  echo "$exit_code" > "$tool_dir/exit_code"

  while read -r file _; do
    [[ -n "$file" ]] || continue
    if ! grep -qxF "$file $(md5sum < "$work_dir/$file")" <<< "$before"; then
      cp "$work_dir/$file" "$tool_dir/$file"
      if [[ "$file" == *.log ]]; then
        sanitize_log "$tool_dir/$file"
      fi
    fi
  done <<< "$after"

  echo "$tool exited with $exit_code, recorded: $(ls "$tool_dir" | tr '\n' ' ')"
  return "$exit_code"
}

mkdir -p "$out_dir"

# Runs the given tool and stops the script if it fails. The failure is recorded like any
# other outcome, but the script exits with the tool's exit code so that a caller notices
# it. Callers that record a failing compile on purpose must ignore that exit code.
run() {
  local exit_code=0
  record "$@" || exit_code=$?
  if [[ $exit_code -ne 0 ]]; then
    echo "warning: $1 failed with exit code $exit_code; later tools were not run" >&2
    exit "$exit_code"
  fi
}

run qbsp "$qbsp_args"
if [[ $run_vis -eq 1 ]]; then
  run vis "$vis_args"
fi
if [[ $run_light -eq 1 ]]; then
  run light "$light_args"
fi
