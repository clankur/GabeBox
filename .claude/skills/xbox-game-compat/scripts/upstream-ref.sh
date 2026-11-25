#!/usr/bin/env bash
# Find the upstream reference implementation of a Win32 symbol (or any pattern)
# in Valve's Wine / Proton / DXVK / vkd3d-proton trees, and print a citable
# permalink pinned to the exact upstream commit.
#
# The clone is blobless and sparse, so a first lookup costs a second and a few
# megabytes. Later lookups in the same tree reuse the cache.
#
#   upstream-ref.sh ImmGetIMEFileNameA
#   upstream-ref.sh CM_Register_Notification --module dlls/cfgmgr32
#   upstream-ref.sh XInputGetState --repo wine --ref proton_11.0 --lines 120
#   upstream-ref.sh --repo proton --path proton --grep 'appid'
#
# Output is evidence for a compatibility note: module, file:line, the bounded
# body, and the permalink. Read upstream for the behavioral contract (argument
# handling, return values, error paths). Do not paste upstream source into this
# repository without recording the component and its license.

set -euo pipefail

repo=wine
ref=
module=
path=
pattern=
lines=80
cache=${XBOX_COMPAT_CACHE:-$HOME/.cache/xbox-game-compat}

usage() {
    sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --repo) repo=$2; shift 2 ;;
        --ref) ref=$2; shift 2 ;;
        --module) module=$2; shift 2 ;;
        --path) path=$2; shift 2 ;;
        --grep) pattern=$2; shift 2 ;;
        --lines) lines=$2; shift 2 ;;
        --cache) cache=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        -*) echo "unknown option: $1" >&2; usage 1 ;;
        *) if [ -z "$pattern" ]; then pattern=$1; else echo "unexpected argument: $1" >&2; usage 1; fi; shift ;;
    esac
done

[ -n "$pattern" ] || usage 1

case "$repo" in
    wine)   url=https://github.com/ValveSoftware/wine.git;         default_ref=proton_11.0 ;;
    proton) url=https://github.com/ValveSoftware/Proton.git;        default_ref=proton_11.0 ;;
    dxvk)   url=https://github.com/doitsujin/dxvk.git;              default_ref=master ;;
    vkd3d)  url=https://github.com/HansKristian-Work/vkd3d-proton.git; default_ref=master ;;
    *) echo "unknown repo: $repo (wine, proton, dxvk, vkd3d)" >&2; exit 2 ;;
esac
ref=${ref:-$default_ref}

tree="$cache/$repo-$ref"
if [ ! -d "$tree/.git" ]; then
    mkdir -p "$cache"
    git clone --quiet --depth 1 --filter=blob:none --sparse --branch "$ref" "$url" "$tree"
    git -C "$tree" sparse-checkout set --no-cone '/*.md' >/dev/null
fi

commit=$(git -C "$tree" rev-parse HEAD)
web=${url%.git}

add_sparse() {
    # The tree was initialized in non-cone mode, so `add` takes the same
    # gitignore-style patterns and must not repeat the mode flag.
    git -C "$tree" sparse-checkout add "$1" >/dev/null
}

# Wine's .spec files map an exported symbol to the DLL that implements it, so a
# bare symbol name is enough to find the module without guessing.
if [ -z "$module" ] && [ -z "$path" ] && [ "$repo" = wine ]; then
    add_sparse '/dlls/*/*.spec'
    spec_hit=$(grep -rln --include='*.spec' -e "[[:space:]]${pattern}(" -e "[[:space:]]${pattern}\$" "$tree/dlls" 2>/dev/null | head -5 || true)
    if [ -n "$spec_hit" ]; then
        echo "# spec declarations"
        while IFS= read -r spec; do
            rel=${spec#"$tree"/}
            grep -n -e "[[:space:]]${pattern}(" -e "[[:space:]]${pattern}\$" "$spec" | sed "s|^|$rel:|"
            module=${rel%/*}
        done <<<"$spec_hit"
        echo
    fi
fi

search_root=$tree
if [ -n "$path" ]; then
    add_sparse "/$path"
    search_root="$tree/$path"
elif [ -n "$module" ]; then
    add_sparse "/$module"
    search_root="$tree/$module"
else
    # No module known: pull the source trees that hold implementations.
    case "$repo" in
        wine)   add_sparse '/dlls' ;;
        proton) add_sparse '/*'; add_sparse '/lsteamclient' ;;
        dxvk)   add_sparse '/src' ;;
        vkd3d)  add_sparse '/libs' ;;
    esac
fi

echo "# $repo @ $ref ($commit)"
echo "# searching ${search_root#"$tree"}"
echo

hits=$(grep -rn --binary-files=without-match \
    --include='*.c' --include='*.h' --include='*.cpp' --include='*.m' \
    --include='*.spec' --include='*.idl' --include='proton' --include='*.py' \
    -e "$pattern" "$search_root" 2>/dev/null | grep -v '/tests/' | head -40 || true)

if [ -z "$hits" ]; then
    echo "no match for '$pattern' in $repo @ $ref"
    echo "Try --module (e.g. --module dlls/user32) or a looser --grep pattern."
    exit 3
fi

echo "# matches"
echo "$hits" | sed "s|^$tree/||"
echo

# A definition line starts at column 0 and carries the symbol: that is the body
# worth reading, and the line range worth citing.
definition=$(echo "$hits" | grep -E ":[0-9]+:[A-Za-z_].*$pattern" | grep -v ';[[:space:]]*$' | head -1 || true)
[ -n "$definition" ] || definition=$(echo "$hits" | head -1)

file=${definition%%:*}
rest=${definition#*:}
start=${rest%%:*}
rel=${file#"$tree"/}
end=$((start + lines))

echo "# $rel:$start"
echo "$web/blob/$commit/$rel#L${start}-L${end}"
echo
sed -n "${start},${end}p" "$file"
