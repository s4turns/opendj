#!/usr/bin/env bash
#
# Installs MilkDrop preset packs and the shared texture pack for the OpenDJ
# visualiser.
#
#   scripts/get-presets.sh              install into ~/.local/share/opendj
#   scripts/get-presets.sh --system     install into /usr/local/share/opendj
#   scripts/get-presets.sh --list       show the packs and their sizes
#
# The presets are not part of this repository. They were released over two
# decades by many authors, almost none under any stated licence, and the
# projectM team keeps them in repositories of their own on that footing. This
# fetches them at your request rather than shipping them, which also keeps a
# hundred megabytes of somebody else's art out of a DJ application's history.
#
# A note on Butterchurn, since it is the obvious thing to ask for: its preset
# packs are the same MilkDrop presets, but converted, with the equations
# turned into JavaScript and the shaders into GLSL. projectM reads the
# original MilkDrop expression language and HLSL, so those files cannot be
# used here, and converting them back would mean reverse-translating two
# languages. The packs below are where Butterchurn's presets came from, and
# they hold about six times as many.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${HOME}/.local/share/opendj"
do_list=0
use_sudo=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) prefix="/usr/local/share/opendj"; shift ;;
        --list)   do_list=1; shift ;;
        --prefix) prefix="$2"; shift 2 ;;
        -h|--help) sed -n '3,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# name|repository|what it is
packs=(
    "cream-of-the-crop|presets-cream-of-the-crop|9795 presets, the large curated collection"
    "projectm-classic|presets-projectm-classic|4188 presets, the set projectM has shipped for years"
    "milkdrop-original|presets-milkdrop-original|552 presets, the ones MilkDrop itself came with"
    "en-d|presets-en-d|40 presets by en-d"
)

texture_pack="presets-milkdrop-texture-pack"

if [[ $do_list -eq 1 ]]; then
    printf '%-20s %s\n' "PACK" "WHAT IT IS"
    for entry in "${packs[@]}"; do
        IFS='|' read -r name _ description <<< "$entry"
        printf '%-20s %s\n' "$name" "$description"
    done
    printf '%-20s %s\n' "textures" "67 images the presets above load by name"
    echo
    echo "Installs into: $prefix"
    exit 0
fi

if ! command -v git >/dev/null 2>&1; then
    echo "git is needed to fetch the presets." >&2
    exit 1
fi

# Writing into somewhere the user owns needs no help; anywhere else does. The
# same test scripts/build.sh makes before it installs.
if [[ ! -w "$(dirname "$prefix")" && ! -w "$prefix" ]]; then
    echo "Installing to $prefix needs root."
    use_sudo="sudo"
fi

$use_sudo mkdir -p "$prefix/presets" "$prefix/textures"

fetch() {
    local repository="$1" destination="$2"

    # A shallow clone, then the history thrown away: what is wanted is the
    # files, and keeping the .git would roughly double what this costs on
    # disk for no benefit to anybody.
    local work
    work="$(mktemp -d)"

    echo "Fetching $repository ..."
    git clone --depth 1 --quiet "https://github.com/projectM-visualizer/$repository.git" "$work/pack"
    rm -rf "$work/pack/.git"

    $use_sudo rm -rf "$destination"
    $use_sudo mkdir -p "$destination"
    $use_sudo cp -r "$work/pack/." "$destination/"
    rm -rf "$work"
}

for entry in "${packs[@]}"; do
    IFS='|' read -r name repository _ <<< "$entry"
    fetch "$repository" "$prefix/presets/$name"
done

# The textures go in one flat directory rather than a tree: projectM is given
# directories to look in, not trees to walk, and the pack arrives with its
# images spread over subfolders.
fetch "$texture_pack" "$prefix/textures/milkdrop"

if [[ -d "$prefix/textures/milkdrop" ]]; then
    $use_sudo find "$prefix/textures/milkdrop" -mindepth 2 -type f \
        \( -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.png' -o -iname '*.dds' -o -iname '*.tga' \) \
        -exec $use_sudo mv -n {} "$prefix/textures/milkdrop/" \; 2>/dev/null || true
fi

preset_count="$(find "$prefix/presets" -name '*.milk' 2>/dev/null | wc -l)"
texture_count="$(find "$prefix/textures" -type f 2>/dev/null | wc -l)"

echo
echo "Installed $preset_count presets and $texture_count textures into $prefix"
echo "Start OpenDJ and press Visuals; click the picture for the next preset."
