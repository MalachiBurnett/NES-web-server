#!/usr/bin/env bash
# Installs the serial<->HTTP bridge (scripts/serial_bridge.py) as a
# systemd service on Ubuntu, so it survives reboots and restarts on
# crash. Point your Cloudflare Tunnel at the HTTP port this exposes.
#
#   sudo ./scripts/install_service.sh
#   sudo ./scripts/install_service.sh --serial-port /dev/ttyACM0 --port 8080
#
# Re-run after a `git pull` to pick up dependency changes; it is safe
# to run repeatedly.
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --serial-port) SERIAL_PORT="$2"; shift 2 ;;
    --host) HTTP_HOST="$2"; shift 2 ;;
    --port) HTTP_PORT="$2"; shift 2 ;;
    --user) RUN_AS_USER="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this with sudo - it installs packages and a systemd unit." >&2
  exit 1
fi

echo "Installing python3-venv..."
apt-get update -qq
apt-get install -y python3 python3-venv >/dev/null

echo "Creating virtualenv at ${VENV_DIR}..."
sudo -u "${RUN_AS_USER}" python3 -m venv "${VENV_DIR}"
sudo -u "${RUN_AS_USER}" "${VENV_DIR}/bin/pip" install --quiet -r "${REPO_ROOT}/requirements.txt"

echo "Writing ${ENV_FILE}..."
cat > "${ENV_FILE}" <<EOF
NES_BRIDGE_SERIAL_PORT=${SERIAL_PORT}
NES_BRIDGE_HOST=${HTTP_HOST}
NES_BRIDGE_PORT=${HTTP_PORT}
EOF

echo "Writing ${SERVICE_FILE}..."
cat > "${SERVICE_FILE}" <<EOF
[Unit]
Description=NES web server serial-to-HTTP bridge
After=network.target

[Service]
Type=simple
User=${RUN_AS_USER}
SupplementaryGroups=dialout
EnvironmentFile=${ENV_FILE}
WorkingDirectory=${REPO_ROOT}
ExecStart=${VENV_DIR}/bin/python3 ${REPO_ROOT}/scripts/serial_bridge.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

echo "Enabling and starting ${SERVICE_NAME}..."
systemctl daemon-reload
systemctl enable "${SERVICE_NAME}" >/dev/null
systemctl restart "${SERVICE_NAME}"

echo
echo "Done. Serial port: ${SERIAL_PORT}, HTTP: ${HTTP_HOST}:${HTTP_PORT}"
echo "Status: systemctl status ${SERVICE_NAME}"
echo "Logs:   journalctl -u ${SERVICE_NAME} -f"
