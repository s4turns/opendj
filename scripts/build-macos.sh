#!/bin/bash
# Configure and build OpenDJ on macOS, independently of the Linux/Windows builds.
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: scripts/build-macos.sh [options]

  --arch arm64|x86_64|universal  Native architecture by default
  --debug | --release           Debug or Release (default: RelWithDebInfo)
  --test                        Run the unit and integration tests
  --run [files...]              Launch the app with optional tracks; pass last
  --deps                        Install CMake and Ninja with Homebrew, then exit
  --clean                       Remove only this architecture/configuration's build
  --jobs N                      Parallel build jobs (default: logical CPU count)
  --deployment-target VERSION   Minimum macOS version (default: 12.6)
  --openvino DIR                Enable stems using DIR/OpenVINOConfig.cmake
  -h, --help                    Show this help

Builds and fetched dependencies stay under build-macos/<arch>/<configuration>.
Intel tests/runs on Apple Silicon require Rosetta. Universal tests run natively;
use --arch x86_64 --test to exercise the Intel version on Apple Silicon.
OpenVINO is disabled unless requested; its libraries must match every requested
architecture. The macOS visualizer backend is not implemented yet.
USAGE
}

fail() { echo "Error: $*" >&2; exit 1; }
require_value() {
    [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || fail "$1 needs a value."
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
architecture=""
config="RelWithDebInfo"
do_clean=0
do_test=0
do_run=0
do_deps=0
jobs=""
deployment_target="${MACOSX_DEPLOYMENT_TARGET:-12.6}"
openvino_dir=""
run_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) require_value "$@"; architecture="$2"; shift 2 ;;
        --debug) config="Debug"; shift ;;
        --release) config="Release"; shift ;;
        --test) do_test=1; shift ;;
        --run) do_run=1; shift; run_args=("$@"); break ;;
        --deps) do_deps=1; shift ;;
        --clean) do_clean=1; shift ;;
        --jobs) require_value "$@"; jobs="$2"; shift 2 ;;
        --deployment-target) require_value "$@"; deployment_target="$2"; shift 2 ;;
        --openvino) require_value "$@"; openvino_dir="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "Unknown option: $1. See --help." ;;
    esac
done

[[ "$(uname -s)" == Darwin ]] || fail "This script requires macOS."
case "$architecture" in
    ""|arm64|x86_64|universal) ;;
    *) fail "--arch must be arm64, x86_64 or universal." ;;
esac
[[ "$deployment_target" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || fail "Invalid macOS deployment target: $deployment_target"
[[ -z "$jobs" || "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer."

command -v xcrun >/dev/null 2>&1 || fail "Install Apple's developer tools: xcode-select --install"
sdk_path="$(xcrun --sdk macosx --show-sdk-path)" || fail "Select an installed Xcode or Command Line Tools with xcode-select."
c_compiler="$(xcrun --sdk macosx --find clang)"
cxx_compiler="$(xcrun --sdk macosx --find clang++)"

if [[ $do_deps -eq 1 ]]; then
    command -v brew >/dev/null 2>&1 || fail "Install Homebrew from https://brew.sh, or install CMake 3.22+ and Ninja yourself."
    brew install cmake ninja
    echo "Build tools installed. RTMP broadcasting additionally needs ffmpeg with libx264 (brew install ffmpeg)."
    exit 0
fi

for tool in cmake git codesign lipo; do
    command -v "$tool" >/dev/null 2>&1 || fail "$tool is missing. Install Apple's developer tools and run scripts/build-macos.sh --deps."
done
if [[ $do_test -eq 1 ]]; then
    command -v ctest >/dev/null 2>&1 || fail "ctest is missing; install CMake."
fi

# Detect the hardware even when the terminal itself is running under Rosetta.
host_architecture="$(uname -m)"
if [[ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" == 1 ]]; then
    host_architecture="arm64"
fi
architecture="${architecture:-$host_architecture}"
case "$architecture" in
    arm64|x86_64) cmake_architectures="$architecture" ;;
    universal) cmake_architectures="arm64;x86_64" ;;
    *) fail "Unsupported native architecture: $architecture" ;;
esac

if [[ $do_run -eq 1 || $do_test -eq 1 ]]; then
    if [[ "$architecture" == arm64 && "$host_architecture" != arm64 ]]; then
        fail "An Intel Mac can build arm64 but cannot run it."
    fi
    if [[ "$architecture" == x86_64 && "$host_architecture" == arm64 ]]; then
        /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || fail "Intel tests/runs require Rosetta. Install Rosetta, then retry."
    fi
fi

build_dir="$repo_root/build-macos/$architecture/$config"
jobs="${jobs:-$(sysctl -n hw.logicalcpu)}"

cmake_args=(
    -S "$repo_root" -B "$build_dir"
    "-DCMAKE_BUILD_TYPE=$config"
    "-DCMAKE_C_COMPILER=$c_compiler"
    "-DCMAKE_CXX_COMPILER=$cxx_compiler"
    "-DCMAKE_OSX_SYSROOT=$sdk_path"
    "-DCMAKE_OSX_ARCHITECTURES=$cmake_architectures"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment_target"
    "-DFETCHCONTENT_BASE_DIR=$build_dir/_deps"
    -DOPENDJ_ENABLE_VISUALIZER=OFF
    -DOPENDJ_BUILD_TESTS=ON
)

# The SDK's SQLite supports both architectures. A Homebrew ARM-only library
# can otherwise be picked up and break an Intel or universal link.
[[ -f "$sdk_path/usr/include/sqlite3.h" && -f "$sdk_path/usr/lib/libsqlite3.tbd" ]] || fail "The selected macOS SDK is missing SQLite."
cmake_args+=(
    "-DSQLite3_INCLUDE_DIR=$sdk_path/usr/include"
    "-DSQLite3_LIBRARY=$sdk_path/usr/lib/libsqlite3.tbd"
)

if [[ -n "$openvino_dir" ]]; then
    [[ -f "$openvino_dir/OpenVINOConfig.cmake" ]] || fail "--openvino must name the directory containing OpenVINOConfig.cmake."
    openvino_dir="$(cd "$openvino_dir" && pwd)"
    cmake_args+=("-DOpenVINO_DIR=$openvino_dir" -DCMAKE_DISABLE_FIND_PACKAGE_OpenVINO=OFF -DCMAKE_REQUIRE_FIND_PACKAGE_OpenVINO=ON)
else
    cmake_args+=(-DCMAKE_DISABLE_FIND_PACKAGE_OpenVINO=ON -DCMAKE_REQUIRE_FIND_PACKAGE_OpenVINO=OFF)
fi

if [[ $do_clean -eq 1 ]]; then
    echo "Removing $build_dir"
    rm -rf "$build_dir"
fi

# Preserve the generator when returning to an existing build tree.
if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    if command -v ninja >/dev/null 2>&1; then
        cmake_args+=(-G Ninja)
    else
        cmake_args+=(-G "Unix Makefiles")
    fi
fi

echo "Configuring macOS $architecture ($config, minimum macOS $deployment_target)..."
cmake "${cmake_args[@]}"
echo "Building with $jobs jobs..."
cmake --build "$build_dir" --config "$config" --parallel "$jobs"

app_bundle="$build_dir/OpenDJ_artefacts/$config/OpenDJ.app"
binary="$app_bundle/Contents/MacOS/OpenDJ"
[[ -x "$binary" ]] || fail "Build finished but $binary was not found."
for requested_architecture in ${cmake_architectures//;/ }; do
    lipo "$binary" -verify_arch "$requested_architecture"
done

# Local development signing. Public distribution needs Developer ID signing
# and notarization as a separate release step.
codesign --force --sign - "$app_bundle"
codesign --verify --strict "$app_bundle"
echo "Built $app_bundle"

if [[ $do_test -eq 1 ]]; then
    ctest --test-dir "$build_dir" -C "$config" --output-on-failure
fi

if [[ $do_run -eq 1 ]]; then
    # Explicitly prefer arm64 for a universal app, including in a Rosetta shell.
    run_architecture="$architecture"
    [[ "$run_architecture" != universal ]] || run_architecture="$host_architecture"
    exec /usr/bin/arch "-$run_architecture" "$binary" ${run_args[@]+"${run_args[@]}"}
fi
