#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Debug mode for the Android port: Blender's own debug flags without rebuilding, logs filtered to
# the app, crashes, and a report bundle to attach to a bug.
#
#   build_files/android/debug.sh status
#   build_files/android/debug.sh on events            # a preset, see `presets`
#   build_files/android/debug.sh on -- --debug-wm --log "wm.*"
#   build_files/android/debug.sh off
#   build_files/android/debug.sh log                  # follow Blender's logcat (Ctrl+C to stop)
#   build_files/android/debug.sh crash                # the last native crash, with its backtrace
#   build_files/android/debug.sh report [note]        # bundle everything into a .tar.gz
#   build_files/android/debug.sh restart
#   build_files/android/debug.sh build [build.py args] # a debuggable APK, installed and started
#
# How the flags reach Blender: the native glue reads launch arguments from `blender_args.txt` in
# the app's internal files directory and then in its external one,
# /sdcard/Android/data/org.blender.blender/files (GHOST_AndroidMain.cc). adb can write the
# external one on any build, release or debuggable, so `on` and `off` need no rebuild -- only a
# restart, which they do. Blender's stdout and stderr already go to logcat under the tag
# "blender", which is where `--debug-*` and `--log` output appears.
#
# Environment: ADB (path to adb), SERIAL (device serial), BUILD_BASE (report location).

set -u

PKG="org.blender.blender"
ACTIVITY="$PKG/.BlenderActivity"
EXT_DIR="/sdcard/Android/data/$PKG/files"
ARGS_FILE="$EXT_DIR/blender_args.txt"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_BASE="${BUILD_BASE:-/root/blender_android_deps}"

die() {
  echo "debug.sh: $*" >&2
  exit 1
}

# adb: $ADB, then the Windows SDK seen from WSL (the device is usually paired there), then the
# Linux SDK, then PATH.
find_adb() {
  if [ -n "${ADB:-}" ]; then
    echo "$ADB"
    return
  fi
  local candidate
  for candidate in /mnt/c/Users/*/AppData/Local/Android/Sdk/platform-tools/adb.exe \
    "${ANDROID_HOME:-/root/android-sdk}/platform-tools/adb"; do
    if [ -x "$candidate" ]; then
      echo "$candidate"
      return
    fi
  done
  command -v adb || die "no adb found; set ADB=/path/to/adb"
}

ADB_BIN="$(find_adb)"

# The first attached device, unless SERIAL picks one.
find_serial() {
  if [ -n "${SERIAL:-}" ]; then
    echo "$SERIAL"
    return
  fi
  "$ADB_BIN" devices | tr -d '\r' | awk 'NR > 1 && $2 == "device" {print $1; exit}'
}

DEVICE="$(find_serial)"
[ -n "$DEVICE" ] || [ "${1:-}" = "presets" ] || [ "${1:-}" = "build" ] ||
  die "no device attached (adb devices is empty); pair it or set SERIAL"

adb_() {
  "$ADB_BIN" -s "$DEVICE" "$@"
}

# adb.exe from WSL hands back CRLF.
adb_out() {
  adb_ "$@" | tr -d '\r'
}

app_pid() {
  adb_out shell pidof "$PKG" | awk '{print $1}'
}

preset_flags() {
  case "$1" in
    events) echo "--debug-events --debug-handlers" ;;
    wm) echo "--debug-wm --debug-events" ;;
    gpu) echo "--debug-gpu --log gpu.*" ;;
    vulkan) echo "--debug-gpu --log gpu.vulkan.*" ;;
    workarounds) echo "--debug-gpu-force-workarounds" ;;
    python) echo "--debug-python" ;;
    depsgraph) echo "--debug-depsgraph" ;;
    log) echo "--log *" ;;
    all) echo "--debug-all" ;;
    *) return 1 ;;
  esac
}

list_presets() {
  local name
  for name in events wm gpu vulkan workarounds python depsgraph log all; do
    printf '  %-12s %s\n' "$name" "$(preset_flags "$name")"
  done
}

restart_app() {
  adb_ shell am force-stop "$PKG"
  adb_ shell am start -n "$ACTIVITY" >/dev/null
  echo "restarted $PKG"
}

current_args() {
  adb_out shell "cat '$ARGS_FILE' 2>/dev/null"
}

cmd_status() {
  echo "adb:        $ADB_BIN"
  echo "device:     $DEVICE  ($(adb_out shell getprop ro.product.model), Android $(adb_out shell getprop ro.build.version.release))"
  local info
  info="$(adb_out shell dumpsys package "$PKG")"
  echo "version:    $(echo "$info" | sed -n 's/^ *versionName=//p' | head -1)"
  echo "installed:  $(echo "$info" | sed -n 's/^ *lastUpdateTime=//p' | head -1)"
  if echo "$info" | grep -q "flags=\[.*DEBUGGABLE"; then
    echo "debuggable: yes (run-as works)"
  else
    echo "debuggable: no (debug.sh build makes one; on/off work either way)"
  fi
  local pid
  pid="$(app_pid)"
  echo "running:    ${pid:-no}"
  local args
  args="$(current_args)"
  echo "debug args: ${args:-none}"
}

cmd_on() {
  local flags
  if [ "${1:-}" = "--" ]; then
    shift
    flags="$*"
  elif [ -n "${1:-}" ]; then
    flags="$(preset_flags "$1")" || die "unknown preset '$1'; see: debug.sh presets"
  else
    die "on needs a preset or -- followed by flags"
  fi
  [ -n "$flags" ] || die "no flags given"
  # Written through a here-string on the device side, so the shell there does no globbing on `*`.
  adb_ shell "mkdir -p '$EXT_DIR' && cat > '$ARGS_FILE'" <<<"$flags"
  echo "debug args: $(current_args)"
  restart_app
  echo "follow the output with: build_files/android/debug.sh log"
}

cmd_off() {
  adb_ shell "rm -f '$ARGS_FILE'"
  echo "debug args removed"
  restart_app
}

cmd_log() {
  local pid
  pid="$(app_pid)"
  if [ -n "$pid" ]; then
    echo "following pid $pid (Ctrl+C to stop)" >&2
    adb_ logcat -v threadtime --pid="$pid"
  else
    echo "Blender is not running; following its tags instead (Ctrl+C to stop)" >&2
    adb_ logcat -v threadtime blender:V blender-surface:V blender-swapchain:V \
      AndroidRuntime:E DEBUG:V libc:F '*:S'
  fi
}

cmd_crash() {
  local crash
  crash="$(adb_out logcat -b crash -d -v threadtime)"
  if [ -z "$crash" ]; then
    echo "no crash in the crash buffer since it was last cleared"
    return
  fi
  # The last crash: from its last "*** ***" header to the end.
  echo "$crash" | awk '/\*\*\* \*\*\* \*\*\*/ {buf=""} {buf = buf $0 "\n"} END {printf "%s", buf}'
}

cmd_report() {
  local note="${*:-}"
  local stamp dir
  stamp="$(date +%Y%m%d-%H%M%S)"
  dir="$BUILD_BASE/debug_reports/blender-android-$stamp"
  mkdir -p "$dir" || die "cannot create $dir"
  local pid
  pid="$(app_pid)"

  {
    echo "report:   $stamp"
    [ -n "$note" ] && echo "note:     $note"
    echo "commit:   $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null) ($(git -C "$REPO_ROOT" branch --show-current 2>/dev/null))"
    cmd_status
    echo "soc:      $(adb_out shell getprop ro.soc.manufacturer) $(adb_out shell getprop ro.soc.model)"
  } >"$dir/summary.txt" 2>&1

  adb_out logcat -d -v threadtime >"$dir/logcat_all.txt"
  if [ -n "$pid" ]; then
    adb_out logcat -d -v threadtime --pid="$pid" >"$dir/logcat_blender.txt"
    adb_out shell dumpsys meminfo "$PKG" >"$dir/meminfo.txt"
    adb_out shell dumpsys gfxinfo "$PKG" >"$dir/gfxinfo.txt"
  fi
  adb_out logcat -b crash -d -v threadtime >"$dir/logcat_crash.txt"
  # Held keys, stuck modifiers and attached keyboards: the stuck Ctrl of 2026-10-14 was in here.
  adb_out shell dumpsys input >"$dir/input.txt"
  adb_out shell dumpsys input_method >"$dir/input_method.txt"
  adb_out shell dumpsys window | grep -E "mCurrentFocus|mFocusedApp" >"$dir/focus.txt"
  current_args >"$dir/debug_args.txt"
  adb_ exec-out screencap -p >"$dir/screen.png"

  tar -czf "$dir.tar.gz" -C "$(dirname "$dir")" "$(basename "$dir")"
  echo "report: $dir"
  echo "bundle: $dir.tar.gz ($(du -h "$dir.tar.gz" | cut -f1))"
}

cmd_build() {
  # A debuggable APK: run-as works, so the internal files directory, the databases and a native
  # debugger become reachable. Never for a release.
  export BUILD_BASE
  export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
  export ANDROID_HOST_CC="${ANDROID_HOST_CC:-gcc-15}" ANDROID_HOST_CXX="${ANDROID_HOST_CXX:-g++-15}"
  cd "$REPO_ROOT" || exit 1
  python3 build_files/android/build.py full --debuggable --install --run "$@"
}

case "${1:-}" in
  status) cmd_status ;;
  presets) list_presets ;;
  on) shift; cmd_on "$@" ;;
  off) cmd_off ;;
  log) cmd_log ;;
  crash) cmd_crash ;;
  report) shift; cmd_report "$@" ;;
  restart) restart_app ;;
  build) shift; cmd_build "$@" ;;
  *)
    sed -n '6,20p' "$0" | sed 's/^# \{0,1\}//'
    echo
    echo "presets:"
    list_presets
    exit 1
    ;;
esac
