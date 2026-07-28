#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$SCRIPT_DIR/run/dev-sys.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID"
        echo "dev-sys-cloud stopped (PID: $PID)"
    else
        echo "Process not running"
    fi
    rm -f "$PID_FILE"
else
    echo "No PID file found"
    fuser -k 9080/tcp 2>/dev/null && echo "Killed process on port 9080" || echo "No process found"
fi
