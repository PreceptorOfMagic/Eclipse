#!/usr/bin/env bash
# Build Eclipse for LG webOS (compatible with LG C1 and other webOS TVs)
# Requires: Linux or WSL2 with Ubuntu

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
SDK_DIR="${WEBOS_SDK_DIR:-/tmp/arm-webos-linux-gnueabi_sdk-buildroot}"

cd "${PROJECT_ROOT}"

echo "=== Eclipse - Build for LG webOS ==="
echo "Project: ${PROJECT_ROOT}"
echo "Build type: ${BUILD_TYPE}"
echo ""

# Check we are in the project root
if [ ! -f "scripts/webos/easy_build.sh" ]; then
    echo "Error: Run this script from the project root"
    exit 1
fi

# Check dependencies
for cmd in cmake awk gawk; do
    if ! command -v $cmd &>/dev/null; then
        echo "Error: $cmd not found. Install with: sudo apt-get install cmake gawk"
        exit 1
    fi
done

# Initialise only the public dependencies used by the client.
echo "Preparing client dependencies..."
./scripts/bootstrap_client_deps.sh

# Toolchains execute code on the builder. Provision an audited SDK explicitly;
# never download and execute an unverified release archive as a build side effect.
if [ ! -f "${SDK_DIR}/share/buildroot/toolchainfile.cmake" ]; then
    echo "Error: audited webOS SDK not found at ${SDK_DIR}" >&2
    echo "Build/provision the SDK first and set WEBOS_SDK_DIR to its extracted root." >&2
    exit 1
fi
echo "WebOS SDK found at ${SDK_DIR}"

TOOLCHAIN_FILE="${SDK_DIR}/share/buildroot/toolchainfile.cmake"

if [ ! -f "${TOOLCHAIN_FILE}" ]; then
    echo "Error: Toolchain not found at ${TOOLCHAIN_FILE}"
    exit 1
fi

# TWO VARIANTS, INSTALLABLE SIDE BY SIDE (2026-09-07).
#   AURORA_VARIANT=beta ./scripts/webos/build_for_lg.sh   -> com.aurora.gamestream.beta "Eclipse Beta"
#   (unset, or =live)                                     -> com.aurora.gamestream      "Eclipse"
# webOS keys the install tree, $HOME and therefore conf/ off the app id, so the two carry
# SEPARATE settings, hosts.ini and pairing with no migration and no risk to the live install.
# The build dir is per-variant too: sharing one would relink the other variant's objects against
# a different APPID and silently produce a binary that identifies as the wrong app.
VARIANT="${AURORA_VARIANT:-live}"
case "$VARIANT" in
    beta) APP_ID="com.aurora.gamestream.beta"; APP_TITLE="Eclipse Beta" ;;
    live) APP_ID="com.aurora.gamestream";      APP_TITLE="Eclipse" ;;
    *) echo "Error: AURORA_VARIANT must be 'live' or 'beta' (got '$VARIANT')"; exit 1 ;;
esac
# The env var easy_build.sh reads is CMAKE_BINARY_DIR. LIVE keeps the historic default so its
# incremental cache -- and therefore everything already tested against it -- is untouched.
if [ "$VARIANT" = "beta" ]; then
    export CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR:-build/webos-beta}"
fi
echo ""
echo "Variant: ${VARIANT}   app id: ${APP_ID}   title: ${APP_TITLE}"
echo "Running build..."
# Every ExternalProject receives CMAKE_TOOLCHAIN_FILE. The SDK does not interpret
# CMAKE_TOOLCHAIN_ARGS, so put path maps in a wrapper that reaches every compiler.
# Keep the SDK's original defaults, including large-file flags and cross compiler.
BUILD_DIR="${CMAKE_BINARY_DIR:-build/webos-easy_build}"
mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
SDK_DIR="$(cd "$SDK_DIR" && pwd)"
# CMake's generated toolchain includes cannot round-trip quotes/backslashes,
# dollar expansions or list separators. Fail clearly for these builder paths;
# bracket quoting preserves spaces and Unicode in supported paths.
for path in "$PROJECT_ROOT" "$BUILD_DIR" "$SDK_DIR"; do
    case "$path" in *';'*|*'"'*|*'\'*|*'$'*|*']========]'*|*"
"*|*"$(printf '\r')"*) echo "Error: unsupported CMake path: $path" >&2; exit 1 ;; esac
done
TOOLCHAIN_FILE="$BUILD_DIR/eclipse-release-toolchain.cmake"
cat > "$TOOLCHAIN_FILE" << EOF
include([========[${SDK_DIR}/share/buildroot/toolchainfile.cmake]========])
add_compile_options(
    [========[-ffile-prefix-map=${PROJECT_ROOT}=/src]========]
    [========[-ffile-prefix-map=${BUILD_DIR}=/build]========]
    [========[-ffile-prefix-map=${SDK_DIR}=/toolchain]========])
EOF
export TOOLCHAIN_FILE
# AURORA_VENDOR_DIAG is forced OFF: a CMake option latches in the cache, so one diagnostic
# build in this build dir would otherwise make every later ipk a diagnostic one.
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DAURORA_VENDOR_DIAG=OFF \
    -DWEBOS_APPINFO_ID="${APP_ID}" \
    -DCMAKE_C_COMPILER="${SDK_DIR}/bin/arm-webos-linux-gnueabi-gcc" \
    -DCMAKE_CXX_COMPILER="${SDK_DIR}/bin/arm-webos-linux-gnueabi-g++"

echo ""
echo "=== Build complete! ==="
echo "IPK package generated at: ${PROJECT_ROOT}/dist/"
ls -la "${PROJECT_ROOT}/dist/"/*.ipk 2>/dev/null || true
echo ""
echo "To install on your TV:"
echo "  Enable Developer Mode on the TV, then use webOS Dev Manager on your computer."
echo "  Select the generated IPK in Dev Manager and install it."
echo "  With the CLI: ares-install <exact-ipk-path> -d <configured-device-name>"
echo "  Close Eclipse before upgrading, then relaunch it after installation."
echo ""
