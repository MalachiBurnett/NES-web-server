#!/usr/bin/env bash
# Run by the nes-serial-bridge service each time it starts, before the
# bridge: pull the latest code, rebuild the gateway firmware, and flash it
# to the board - but only if the firmware actually changed. The service
# restarts itself after a crash, and flashing on every start would wear
# out the board's flash in a crash loop.
#
# Best effort throughout: with no network, a failed build or no board,
# the bridge still starts on whatever it had. Configured from the
# service's environment file (/etc/nes-serial-bridge.env):
#
#   NES_BRIDGE_SERIAL_PORT   the board, e.g. /dev/serial/by-id/usb-Arduino...
#   NES_FIRMWARE_FQBN        e.g. arduino:avr:mega:cpu=atmega2560; empty = never flash
#   NES_FIRMWARE_FLAGS       e.g. -DNES_WIRING_RJ45
#
# To flash again even though nothing changed, delete
# build/fw/service/flashed and restart the service.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

log() { echo "update: $*"; }

before=$(git rev-parse --short HEAD)
if git pull --ff-only -q; then
  after=$(git rev-parse --short HEAD)
  [[ "$before" == "$after" ]] && log "code up to date at $after" || log "pulled $before -> $after"
else
  log "git pull failed; carrying on with $before"
fi

if [[ -x .venv/bin/pip ]]; then
  .venv/bin/pip install -q -r requirements.txt || log "pip install failed"
fi

if [[ -z "${NES_FIRMWARE_FQBN:-}" ]]; then
  log "NES_FIRMWARE_FQBN not set: not building or flashing firmware"
  exit 0
fi
if ! command -v arduino-cli >/dev/null; then
  log "arduino-cli not installed: not building or flashing firmware"
  exit 0
fi

build=build/fw/service
extra=()
[[ -n "${NES_FIRMWARE_FLAGS:-}" ]] && extra=(--build-property "compiler.cpp.extra_flags=${NES_FIRMWARE_FLAGS}")

if ! arduino-cli compile --fqbn "$NES_FIRMWARE_FQBN" ${extra[@]+"${extra[@]}"} \
       --build-path "$build" src/firmware/NES_router >/dev/null; then
  log "firmware build failed; leaving the board as it is"
  exit 0
fi

# What is on the board: this firmware, for this board and wiring, on this port.
image=$(ls "$build"/NES_router.ino.hex "$build"/NES_router.ino.bin 2>/dev/null | head -n 1)
stamp="$(sha256sum "$image" | cut -d' ' -f1) ${NES_FIRMWARE_FQBN} ${NES_FIRMWARE_FLAGS:-} ${NES_BRIDGE_SERIAL_PORT:-}"
if [[ -f "$build/flashed" && "$(cat "$build/flashed")" == "$stamp" ]]; then
  log "firmware unchanged: not flashing"
  exit 0
fi

if [[ ! -e "${NES_BRIDGE_SERIAL_PORT:-/nonexistent}" ]]; then
  log "no board at ${NES_BRIDGE_SERIAL_PORT:-(unset)}: not flashing"
  exit 0
fi

if arduino-cli upload --fqbn "$NES_FIRMWARE_FQBN" -p "$NES_BRIDGE_SERIAL_PORT" \
     --input-dir "$build" src/firmware/NES_router >/dev/null; then
  echo "$stamp" > "$build/flashed"
  log "flashed the new firmware to $NES_BRIDGE_SERIAL_PORT"
else
  log "upload failed; the bridge will start on the board's old firmware"
fi
exit 0
