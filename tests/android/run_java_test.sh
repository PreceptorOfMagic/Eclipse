#!/usr/bin/env bash
# The app's plain-Java helpers (no Android types) on the host JDK.
#   run_java_test.sh <javac> <java> <work dir> <test class> <sources...>
set -euo pipefail
JAVAC=$1 JAVA=$2 W=$3 CLASS=$4
shift 4
rm -rf "$W" && mkdir -p "$W"
"$JAVAC" -Xlint:all,-options -Werror --release 8 -d "$W" "$@" 2>&1 | grep -v '^Picked up JAVA_TOOL_OPTIONS' || true
[ -f "$W/${CLASS//.//}.class" ] || { echo "compilation failed"; exit 1; }
"$JAVA" -cp "$W" "$CLASS" 2>&1 | grep -v '^Picked up JAVA_TOOL_OPTIONS'
exit "${PIPESTATUS[0]}"
