#!/usr/bin/env bash
#
# Build, flash and read the serial log in one go (macOS / Linux).
#
# macOS counterpart of run-device.ps1.  "pio run -t upload" can leave this board
# in the ROM downloader, so after flashing the script issues a watchdog reset
# (esptool --after watchdog_reset) which boots the freshly flashed application.
#
#   ./tools/run-device.sh                    # env cardputer-adv, 6 s of log
#   ./tools/run-device.sh -e example -s 10   # other env, 10 s of log
#   ./tools/run-device.sh -n                 # just flash + reset + read
#   ./tools/run-device.sh -m                 # no build, no flash: reset + read
#   ./tools/run-device.sh -p /dev/cu.usbmodem1101
#
# Options:
#   -e, --env <name>      PlatformIO environment (default: cardputer-adv)
#   -p, --port <device>   serial port; auto-detected when omitted
#   -s, --seconds <n>     how long to read the serial log (default: 6)
#   -n, --no-build        skip the build step
#   -m, --monitor-only    skip build and upload, only reset and read the log
#   -v, --verbose         show the full build/upload output
#   -h, --help            show this help

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

env_name="cardputer-adv"
port=""
secs=6
do_build=1
monitor_only=0
verbose=0
baud=115200

die() { echo "error: $*" >&2; exit 1; }
info() { echo "== $* =="; }

usage() {
    awk 'NR > 2 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else { exit } }' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -e|--env)     [[ $# -ge 2 ]] || die "--env needs a value"; env_name="$2"; shift 2 ;;
        --env=*)      env_name="${1#*=}"; shift ;;
        -p|--port)    [[ $# -ge 2 ]] || die "--port needs a value"; port="$2"; shift 2 ;;
        --port=*)     port="${1#*=}"; shift ;;
        -s|--seconds) [[ $# -ge 2 ]] || die "--seconds needs a value"; secs="$2"; shift 2 ;;
        --seconds=*)  secs="${1#*=}"; shift ;;
        -n|--no-build) do_build=0; shift ;;
        -m|--monitor-only) monitor_only=1; shift ;;
        -v|--verbose) verbose=1; shift ;;
        -h|--help)    usage 0 ;;
        *)            echo "error: unknown argument: $1" >&2; usage 2 ;;
    esac
done

[[ "$secs" =~ ^[0-9]+$ ]] || die "--seconds must be a whole number of seconds"

# ---------------------------------------------------------------- toolchain --

find_pio() {
    if command -v pio >/dev/null 2>&1; then
        command -v pio
        return 0
    fi
    local candidate
    for candidate in "$HOME/.platformio/penv/bin/pio" "/opt/homebrew/bin/platformio"; do
        [[ -x "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

pio="$(find_pio)" || die "PlatformIO not found. Install it (brew install platformio) or add pio to PATH."

# A Python that can import pyserial - used both for reading the serial port and
# for running esptool.  The pio bundle always ships one; plain python3 usually
# does not, unless the user installed pyserial themselves.
find_python() {
    local candidate
    for candidate in \
        "$HOME/.platformio/penv/bin/python" \
        "/opt/homebrew/opt/platformio/libexec/bin/python3"
    do
        if [[ -x "$candidate" ]] && "$candidate" -c 'import serial' >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    for candidate in /opt/homebrew/Cellar/platformio/*/libexec/bin/python3; do
        if [[ -x "$candidate" ]] && "$candidate" -c 'import serial' >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    for candidate in python3 python; do
        if command -v "$candidate" >/dev/null 2>&1 &&
           "$candidate" -c 'import serial' >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    return 1
}

py_serial="$(find_python || true)"

# --------------------------------------------------------------- serial port --

detect_port() {
    local candidate
    for candidate in \
        /dev/cu.usbmodem* \
        /dev/cu.wchusbserial* \
        /dev/cu.usbserial* \
        /dev/cu.SLAB_USBtoUART* \
        /dev/cu.CH34x* \
        /dev/ttyACM* \
        /dev/ttyUSB*
    do
        # An unmatched glob stays literal, so probe with -e.
        [[ -e "$candidate" ]] && { echo "$candidate"; return 0; }
    done
    return 1
}

if [[ -z "$port" ]]; then
    port="$(detect_port)" || die "no serial device found. Plug the Cardputer in or pass --port <device>."
fi
[[ -e "$port" ]] || die "serial port not found: $port"

# The port re-enumerates after flashing on native USB-CDC boards; wait for it
# to come back before opening it.
wait_for_port() {
    local waited=0
    while [[ ! -e "$port" ]]; do
        [[ "$waited" -ge 60 ]] && return 1
        sleep 0.25
        waited=$((waited + 1))
    done
    return 0
}

# ------------------------------------------------------------------- helpers --

tmp="$(mktemp -t cardputer-flash)"
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT

show_tail() {
    local lines="${1:-40}"
    echo "---- last $lines lines ----" >&2
    tail -n "$lines" "$tmp" >&2
}

# --------------------------------------------------------------------- build --

if [[ "$monitor_only" -eq 0 ]]; then

    if [[ "$do_build" -eq 1 ]]; then
        info "build $env_name"
        if ! "$pio" run -d "$root" -e "$env_name" >"$tmp" 2>&1; then
            show_tail 60
            die "build failed"
        fi
        if [[ "$verbose" -eq 1 ]]; then
            cat "$tmp"
        else
            grep -E 'RAM:|Flash:' "$tmp" | tail -n 2 || true
            grep -E 'SUCCESS|Took' "$tmp" | tail -n 1 || true
        fi
    fi

    # ---------------------------------------------------------------- upload --

    info "upload to $port"
    if ! "$pio" run -d "$root" -e "$env_name" -t upload --upload-port "$port" >"$tmp" 2>&1; then
        show_tail 60
        die "upload failed"
    fi
    if [[ "$verbose" -eq 1 ]]; then
        cat "$tmp"
    else
        grep -E 'SUCCESS|FAILED|Hash of data verified|Writing at' "$tmp" | tail -n 3 || true
    fi

    wait_for_port || die "device did not re-appear on $port after flashing"

else
    info "monitor only - skipping build and upload"
fi

# --------------------------------------------------------------------- reset --
#
# Uploading leaves the board in the ROM downloader on this hardware, so the
# application only starts after an explicit watchdog reset.
#
# esptool ships with the espressif32 platform in two different shapes:
#   platform 6.x and older : tool-esptoolpy/esptool.py (needs a Python with pyserial)
#   platform 8.x and newer : tool-esptool/esptool     (self-contained binary)
# Probe once and reuse whichever one answers.

esptool_cmd=()

pick_esptool() {
    # Newer platform: a standalone executable, no Python involved.
    local native="$HOME/.platformio/packages/tool-esptool/esptool"
    if [[ -x "$native" ]] && "$native" version >/dev/null 2>&1; then
        esptool_cmd=("$native")
        return 0
    fi

    # Older platform: esptool.py driven by a Python that has pyserial.
    local bundled="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
    if [[ -n "$py_serial" && -f "$bundled" ]] &&
       "$py_serial" "$bundled" version >/dev/null 2>&1; then
        esptool_cmd=("$py_serial" "$bundled")
        return 0
    fi

    # Last resort: let PlatformIO resolve the package for us.
    local pkg exe
    for pkg in tool-esptool tool-esptoolpy; do
        for exe in esptool esptool.py; do
            if "$pio" pkg exec -p "$pkg" -- "$exe" version >/dev/null 2>&1; then
                esptool_cmd=("$pio" pkg exec -p "$pkg" -- "$exe")
                return 0
            fi
        done
    done
    return 1
}

reset_board() {
    [[ ${#esptool_cmd[@]} -gt 0 ]] || return 1

    # Right after an upload the board sits in the ROM downloader, so no_reset is
    # the fastest path.  When the application is already running (monitor mode)
    # that cannot connect, and esptool has to drive the reset itself.
    # Note the application's own serial output also breaks the no_reset
    # handshake, which is the second reason to fall back.
    local before attempt
    for before in no_reset default_reset; do
        for attempt in 1 2; do
            if "${esptool_cmd[@]}" --port "$port" --before "$before" \
                   --after watchdog_reset chip_id >/dev/null 2>&1; then
                return 0
            fi
            sleep 0.4
        done
    done
    return 1
}

info "boot application"
if pick_esptool && reset_board; then
    sleep 0.7
else
    echo "warning: could not reset the board automatically." >&2
    echo "         if the log below stays empty, press the reset button once." >&2
fi

# --------------------------------------------------------------------- serial --

read_serial_pyserial() {
    sleep 0.2
    "$py_serial" - "$port" "$baud" "$secs" <<'PY'
import sys
import time

try:
    import serial
except ImportError:
    sys.exit(3)

port, baud, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
try:
    # esptool may still hold the port for a moment after the reset.
    deadline = time.time() + 3.0
    while True:
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            break
        except Exception:
            if time.time() >= deadline:
                raise
            time.sleep(0.2)

    end = time.time() + secs
    pending = b""
    try:
        while time.time() < end:
            chunk = ser.read(512)
            if not chunk:
                continue
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                print(line.decode("utf-8", "replace").rstrip(), flush=True)
    finally:
        ser.close()
    if pending:
        print(pending.decode("utf-8", "replace").rstrip(), flush=True)
except Exception as exc:  # noqa: BLE001 - report and fall back
    sys.stderr.write("serial: %s\n" % exc)
    sys.exit(4)
PY
}

read_serial_stty() {
    sleep 0.2
    if [[ "$(uname -s)" == "Darwin" ]]; then
        stty -f "$port" "$baud" cs8 -cstopb -parenb -echo raw 2>/dev/null || true
    else
        stty -F "$port" "$baud" cs8 -cstopb -parenb -echo raw 2>/dev/null || true
    fi
    local reader
    cat "$port" &
    reader=$!
    sleep "$secs"
    kill "$reader" 2>/dev/null || true
    wait "$reader" 2>/dev/null || true
}

info "serial ($secs s)"
if [[ -n "$py_serial" ]] && read_serial_pyserial; then
    :
else
    if [[ -z "$py_serial" ]]; then
        echo "note: no python with pyserial found, falling back to stty + cat." >&2
    fi
    read_serial_stty
fi
