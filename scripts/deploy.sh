#!/bin/bash
set -e

# Deploy script for dev-sys
# Usage: ./deploy.sh <device_ip> [ssh_user]

DEVICE_IP="${1:?Usage: ./deploy.sh <device_ip> [ssh_user]}"
SSH_USER="${2:-root}"
BINARY_PATH="build/debug-host/bin/dev-sys"
CONFIG_DIR="config"

echo "=== Deploying dev-sys to ${SSH_USER}@${DEVICE_IP} ==="

# Stop existing service
ssh "${SSH_USER}@${DEVICE_IP}" "systemctl stop dev-sys || true"

# Copy binary
scp "$BINARY_PATH" "${SSH_USER}@${DEVICE_IP}:/usr/bin/dev-sys"

# Copy configs
ssh "${SSH_USER}@${DEVICE_IP}" "mkdir -p /etc/dev-sys/certs"
scp "${CONFIG_DIR}/device_config.json" "${SSH_USER}@${DEVICE_IP}:/etc/dev-sys/"
scp "${CONFIG_DIR}/mqtt_config.json" "${SSH_USER}@${DEVICE_IP}:/etc/dev-sys/"

# Start service
ssh "${SSH_USER}@${DEVICE_IP}" "systemctl start dev-sys"

echo "=== Deploy complete ==="
