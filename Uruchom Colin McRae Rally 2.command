#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/OpenCMR2-macOS-arm64/opencmr2" --data "$SCRIPT_DIR/Colin McRae Rally 2/Colin McRae Rally 2"
