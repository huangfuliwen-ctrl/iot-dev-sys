#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$SCRIPT_DIR/run/dev-sys.pid"

echo "═══════════════════════════════════════════════"
if [ -f "$PID_FILE" ] && kill -0 $(cat "$PID_FILE") 2>/dev/null; then
    PID=$(cat "$PID_FILE")
    echo "  dev-sys-cloud    RUNNING (PID: $PID)"
    echo "  API:  http://127.0.0.1:9080/api/v1/health"
    echo "  DB:   $DEV_SYS_DB"
    echo "───────────────────────────────────────────────"
    curl -s http://127.0.0.1:9080/api/v1/health 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "  (API unreachable)"
else
    echo "  dev-sys-cloud    STOPPED"
    echo "  启动: bash deploy/start.sh"
fi
echo "═══════════════════════════════════════════════"
