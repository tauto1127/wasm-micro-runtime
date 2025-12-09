#!/usr/bin/env bash

# 使い方:
#   ./signal_iwasm.sh USR1
#   ./signal_iwasm.sh USR2

set -eu

if [ $# -ne 1 ]; then
  echo "Usage: $0 USR1|USR2" >&2
  exit 1
fi

SIGNAL="$1"

if [ "$SIGNAL" != "USR1" ] && [ "$SIGNAL" != "USR2" ]; then
  echo "Error: signal must be USR1 or USR2" >&2
  exit 1
fi

# iwasm プロセスの PID を取得
PID=$(pgrep -u "$USER" -f "iwasm" | head -n 1 || true)

if [ -z "$PID" ]; then
  echo "Error: iwasm process not found." >&2
  exit 1
fi

echo "Sending SIG$SIGNAL to iwasm (PID=$PID)..."
kill -"$SIGNAL" "$PID"
echo "Done."

