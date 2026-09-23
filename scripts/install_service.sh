#!/usr/bin/env bash
# Installs the serial<->HTTP bridge (scripts/serial_bridge.py) as a
# systemd service on Ubuntu, so it survives reboots and restarts on
# crash. Point your Cloudflare Tunnel at the HTTP port this exposes.
#
# Each time the service starts it first runs scripts/update_firmware.sh:
# git pull, rebuild the gateway firmware, and flash the board if the
# firmware changed. So `sudo systemctl restart nes-serial-bridge` deploys
# whatever is on the branch.
#
#   sudo bash scripts/install_service.sh --serial-port /dev/ttyACM0 --port 8080
#
# Options:
#   --serial-port PATH       the gateway (default /dev/ttyACM0)
#   --host ADDR --port N     where the bridge serves HTTP (default 127.0.0.1:8080)
#   --user NAME              who the service runs as (default: whoever ran sudo)
#   --fqbn FQBN              board to build for (default: an Arduino Mega 2560)
#   --firmware-flags FLAGS   e.g. "-DNES_WIRING_RJ45" (see docs/mega.md)
#   --no-firmware            never build or flash; just pull and run the bridge
#
# Safe to run repeatedly; re-run it to change any of the above.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_NAME="nes-serial-bridge"
ENV_FILE="/etc/${SERVICE_NAME}.env"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
VENV_DIR="${REPO_ROOT}/.venv"

SERIAL_PORT="/dev/ttyACM0"
HTTP_HOST="127.0.0.1"
HTTP_PORT="8080"
RUN_AS_USER="${SUDO_USER:-$(whoami)}"
FQBN="arduino:avr:mega:cpu=atmega2560"
FIRMWARE_FLAGS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --serial-port) SERIAL_PORT="$2"; shift 2 ;;
    --host) HTTP_HOST="$2"; shift 2 ;;
    --port) HTTP_PORT="$2"; shift 2 ;;
    --user) RUN_AS_USER="$2"; shift 2 ;;
    --fqbn) FQBN="$2"; shift 2 ;;
    --firmware-flags) FIRMWARE_FLAGS="$2"; shift 2 ;;
    --no-firmware) FQBN=""; shift ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this with sudo - it installs packages and a systemd unit." >&2
  exit 1
fi

echo "Installing python3-venv..."
apt-get update -qq
apt-get install -y python3 python3-venv curl >/dev/null

echo "Creating virtualenv at ${VENV_DIR}..."
sudo -u "${RUN_AS_USER}" python3 -m venv "${VENV_DIR}"
sudo -u "${RUN_AS_USER}" "${VENV_DIR}/bin/pip" install --quiet -r "${REPO_ROOT}/requirements.txt"

if [[ -n "${FQBN}" ]]; then
  if ! command -v arduino-cli >/dev/null; then
    echo "Installing arduino-cli to /usr/local/bin..."
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/local/bin sh >/dev/null
  fi
  CORE="$(echo "${FQBN}" | cut -d: -f1,2)"
  echo "Installing the ${CORE} core for ${RUN_AS_USER}..."
  sudo -u "${RUN_AS_USER}" -H arduino-cli core update-index >/dev/null
  sudo -u "${RUN_AS_USER}" -H arduino-cli core install "${CORE}" >/dev/null
fi

echo "Writing ${ENV_FILE}..."
cat > "${ENV_FILE}" <<EOF
NES_BRIDGE_SERIAL_PORT=${SERIAL_PORT}
NES_BRIDGE_HOST=${HTTP_HOST}
NES_BRIDGE_PORT=${HTTP_PORT}
NES_FIRMWARE_FQBN=${FQBN}
NES_FIRMWARE_FLAGS=${FIRMWARE_FLAGS}
EOF

echo "Writing ${SERVICE_FILE}..."
cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=NES web server serial-to-HTTP bridge
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=${RUN_AS_USER}
SupplementaryGroups=dialout
EnvironmentFile=${ENV_FILE}
WorkingDirectory=${REPO_ROOT}
# Pull, rebuild, and flash the board if its firmware changed. The leading
# "-" starts the bridge even if this fails.
ExecStartPre=-/bin/bash ${REPO_ROOT}/scripts/update_firmware.sh
ExecStart=${VENV_DIR}/bin/python3 ${REPO_ROOT}/scripts/serial_bridge.py
Restart=on-failure
RestartSec=10
TimeoutStartSec=600

[Install]
WantedBy=multi-user.target
EOF

echo "Enabling and starting ${SERVICE_NAME} (the first start builds the firmware, so give it a minute)..."
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}" >/dev/null
systemctl restart "${SERVICE_NAME}"

echo
echo "Done. Serial port: ${SERIAL_PORT}, HTTP: ${HTTP_HOST}:${HTTP_PORT}"
echo "Firmware: ${FQBN:-not managed} ${FIRMWARE_FLAGS}"
echo "Status: systemctl status ${SERVICE_NAME}"
echo "Logs:   journalctl -u ${SERVICE_NAME} -f"
echo "Deploy: sudo systemctl restart ${SERVICE_NAME}   (pulls, rebuilds, reflashes if changed)"
