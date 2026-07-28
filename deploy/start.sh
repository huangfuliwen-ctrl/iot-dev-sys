#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BINARY="$PROJECT_DIR/build/bin/dev-sys-cloud"
PID_FILE="$SCRIPT_DIR/run/dev-sys.pid"
LOG_FILE="$SCRIPT_DIR/log/dev-sys.log"
CONFIG_DIR="$SCRIPT_DIR/config"

mkdir -p "$SCRIPT_DIR/run" "$SCRIPT_DIR/log"

if [ -f "$PID_FILE" ] && kill -0 $(cat "$PID_FILE") 2>/dev/null; then
    echo "dev-sys-cloud already running (PID: $(cat $PID_FILE))"
    exit 0
fi

export DEV_SYS_DB="${DEV_SYS_DB:-postgresql://devsys:devsys@127.0.0.1:5432/devsys_cloud}"
nohup "$BINARY" > "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"
echo "dev-sys-cloud started (PID: $!)"
