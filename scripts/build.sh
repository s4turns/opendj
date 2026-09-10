#!/usr/bin/env bash
#
# Configure and build OpenDJ on Linux.
#
#   scripts/build.sh                  build in RelWithDebInfo
#   scripts/build.sh --debug          build in Debug
#   scripts/build.sh --clean          throw the build directory away first
#   scripts/build.sh --deps           install the system packages and exit
#   scripts/build.sh --run [files]    build, then launch with those tracks loaded
#   scripts/build.sh --install        build, then install (asks for sudo)
#
# Installs under /usr/local by default; set PREFIX to change it, for example
#   PREFIX=~/.local scripts/build.sh --install
# which needs no sudo at all.
#
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build"
config="RelWithDebInfo"
do_clean=0
do_run=0
do_deps=0
do_install=0
prefix="${PREFIX:-/usr/local}"
run_args=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   config="Debug"; shift ;;
        --release) config="Release"; shift ;;
        --clean)   do_clean=1; shift ;;
        --deps)    do_deps=1; shift ;;
        --install) do_install=1; shift ;;
        --run)     do_run=1; shift; run_args=("$@"); break ;;
        -h|--help) sed -n '3,16p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 0 ;;
        *)         echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Dependencies
#
# JUCE needs a fair pile of X11 and audio headers. The package names differ per
# distribution, so name them explicitly rather than guessing from a generic list.
# ---------------------------------------------------------------------------
install_dependencies() {
    if command -v dnf >/dev/null 2>&1; then
        # Fedora, RHEL and derivatives.
        #
        # Fedora 40 and newer ship JACK through PipeWire, and the PipeWire
        # package conflicts with the classic one, so take whichever is present
        # rather than insisting on either.
        local jack_package="pipewire-jack-audio-connection-kit-devel"

        if ! dnf list --available "$jack_package" >/dev/null 2>&1 \
           && ! rpm -q "$jack_package" >/dev/null 2>&1; then
            jack_package="jack-audio-connection-kit-devel"
        fi

        sudo dnf install -y \
            gcc-c++ cmake ninja-build git pkgconf-pkg-config \
            alsa-lib-devel "$jack_package" sqlite-devel \
            freetype-devel fontconfig-devel \
            libX11-devel libXext-devel libXinerama-devel \
            libXrandr-devel libXcursor-devel libXcomposite-devel \
            mesa-libGL-devel

    elif command -v apt-get >/dev/null 2>&1; then
        # Debian, Ubuntu and derivatives.
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
            build-essential cmake ninja-build git pkg-config \
            libasound2-dev libjack-jackd2-dev libsqlite3-dev \
            libfreetype-dev libfontconfig1-dev \
            libx11-dev libxext-dev libxinerama-dev \
            libxrandr-dev libxcursor-dev libxcomposite-dev \
            libgl1-mesa-dev

    elif command -v pacman >/dev/null 2>&1; then
        # Arch and derivatives.
        sudo pacman -S --needed --noconfirm \
            base-devel cmake ninja git pkgconf \
            alsa-lib jack2 sqlite freetype2 fontconfig \
            libx11 libxext libxinerama libxrandr libxcursor libxcomposite mesa

    else
        cat >&2 <<'MESSAGE'
No supported package manager found (dnf, apt-get or pacman).

Install the equivalents of these by hand: a C++20 compiler, CMake 3.22 or newer,
Ninja, pkg-config, and the development headers for ALSA, JACK, SQLite, FreeType,
Fontconfig, libX11, Xext, Xinerama, Xrandr, Xcursor, Xcomposite and OpenGL.

SQLite is optional: without it the build fetches and compiles the public domain
amalgamation instead, which is what happens on Windows.
MESSAGE
        exit 1
    fi
}

if [[ $do_deps -eq 1 ]]; then
    install_dependencies
    echo "Dependencies installed."
    exit 0
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
for tool in cmake git; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is not installed. Run: scripts/build.sh --deps" >&2
        exit 1
    fi
done

if [[ $do_clean -eq 1 ]]; then
    echo "Removing $build_dir"
    rm -rf "$build_dir"
fi

generator_args=()
if command -v ninja >/dev/null 2>&1; then
    generator_args=(-G Ninja)
fi

echo "Configuring ($config)..."
cmake -S "$repo_root" -B "$build_dir" "${generator_args[@]}" \
    -DCMAKE_BUILD_TYPE="$config" -DCMAKE_INSTALL_PREFIX="$prefix"

echo "Building..."
cmake --build "$build_dir" --parallel "$(nproc)"

binary="$build_dir/OpenDJ_artefacts/$config/OpenDJ"
[[ -x "$binary" ]] || binary="$build_dir/OpenDJ_artefacts/OpenDJ"

if [[ ! -x "$binary" ]]; then
    echo "Build finished but the OpenDJ binary was not where it was expected." >&2
    exit 1
fi

echo "Built $binary"

if [[ $do_install -eq 1 ]]; then
    # Writing into somewhere the user owns needs no help; anywhere else does.
    if [[ -w "$prefix" ]] || [[ ! -e "$prefix" && -w "$(dirname "$prefix")" ]]; then
        cmake --install "$build_dir" --component opendj
    else
        echo "Installing to $prefix needs root."
        sudo cmake --install "$build_dir" --component opendj
    fi

    # Without this the launcher does not notice the new entry until the next
    # login, which looks like the install having silently failed.
    if command -v update-desktop-database >/dev/null 2>&1; then
        "$([[ -w "$prefix" ]] || echo sudo)" update-desktop-database "$prefix/share/applications" 2>/dev/null || true
    fi

    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        "$([[ -w "$prefix" ]] || echo sudo)" gtk-update-icon-cache -qtf "$prefix/share/icons/hicolor" 2>/dev/null || true
    fi

    echo "Installed to $prefix"
fi

if [[ $do_run -eq 1 ]]; then
    exec "$binary" "${run_args[@]}"
fi
