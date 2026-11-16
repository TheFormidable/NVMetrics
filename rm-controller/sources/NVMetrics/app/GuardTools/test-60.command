#!/bin/bash
# Attended 4-second RM test. Never runs as root without Terminal authentication.
cd "$(dirname "$0")" || exit 1
/usr/bin/env python3 "./run_guard.py" --run --target 60
code=$?
echo ""
read -r -p "Pulsa Enter para cerrar Terminal… " _
exit "$code"
