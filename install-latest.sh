#!/usr/bin/env bash
# Install the newest Proton-JE release into Steam as one tool with a fixed name, "Proton-JE Latest",
# the way ProtonPlus installs a "Latest" runner: running the script again replaces the tool in place,
# so a game, or Steam's default, that is set to it keeps working across updates.
#
#   bash install-latest.sh                                   # the newest release
#   bash install-latest.sh --tag proton-je-11.0-20261008-1   # a particular one, under the same name
#
# Needs curl, tar, xz and sha512sum. The download and the previous install are kept until the new
# one is verified and in place.

set -euo pipefail

repo=jegglest/proton-je
name="Proton-JE Latest"
tag=
dest=
force=0

usage() {
    cat <<USAGE
Usage: ${0##*/} [--tag TAG] [--dir DIR] [--name NAME] [--force]

Downloads a release of https://github.com/$repo, checks its sha512 sum and installs it into
Steam's compatibilitytools.d under one fixed tool name, replacing the previous install.

  --tag TAG    release to install (default: the newest one that is not a pre-release)
  --dir DIR    Steam's compatibilitytools.d (default: found from the Steam install)
  --name NAME  tool name, also the directory name (default: "$name")
  --force      reinstall even when that release is already installed
USAGE
}

die() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case $1 in
        --tag|--dir|--name)
            [ $# -ge 2 ] || die "$1 needs a value"
            case $1 in --tag) tag=$2 ;; --dir) dest=$2 ;; --name) name=$2 ;; esac
            shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
[ -n "$name" ] || die "the tool name must not be empty"
case $name in */*) die "the tool name must not contain a slash" ;; esac

for t in curl tar xz sha512sum; do
    command -v "$t" >/dev/null || die "$t is needed"
done

# Native Steam links ~/.steam/root to its install; Flatpak Steam lives under ~/.var/app.
if [ -z "$dest" ]; then
    for steam in "$HOME/.steam/root" "$HOME/.var/app/com.valvesoftware.Steam/data/Steam" "$HOME/.local/share/Steam"; do
        if [ -d "$steam" ]; then
            dest=$steam/compatibilitytools.d
            break
        fi
    done
    [ -n "$dest" ] || die "no Steam install found; pass --dir <compatibilitytools.d>"
fi
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)
tool=$dest/$name

# GitHub's "latest" is the newest release that is neither a draft nor a pre-release, the one
# ProtonPlus installs as Latest too; the page redirects to the release's tag.
if [ -z "$tag" ]; then
    url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$repo/releases/latest") \
        || die "could not find the latest release at https://github.com/$repo/releases"
    tag=${url##*/releases/tag/}
    [ -n "$tag" ] && [ "$tag" != "$url" ] || die "no release found at https://github.com/$repo/releases"
fi

installed=
if [ -r "$tool/version" ]; then
    read -r _ installed _ < "$tool/version" || true   # "<build time> <release>"
fi
if [ "$installed" = "$tag" ] && [ "$force" = 0 ]; then
    echo "$tag is already installed as \"$name\" in $dest"
    exit 0
fi

staging=$dest/$name.partial
old=
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        if [ -n "$old" ] && [ -e "$old" ] && ! [ -e "$tool" ]; then
            mv -- "$old" "$tool"   # the swap failed half way: put the previous install back
        fi
        rmdir -- "$staging" 2>/dev/null || true   # keep it only if it holds a download
        [ -d "$staging" ] && echo "${0##*/}: failed; what was downloaded stays in $staging for the next run" >&2
    fi
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$staging"
cd "$staging"
base=https://github.com/$repo/releases/download/$tag
# A verified archive from an earlier, interrupted run is reused; anything else is fetched afresh.
if ! { [ -f "$tag.sha512sum" ] && sha512sum -c --quiet --status "$tag.sha512sum"; } 2>/dev/null; then
    rm -f -- ./*.tar.xz ./*.sha512sum
    progress=-sS
    [ -t 2 ] && progress=--progress-bar
    echo "Downloading $tag"
    curl -fL --retry 3 -sS -o "$tag.sha512sum" "$base/$tag.sha512sum" || die "no release $tag at $base"
    curl -fL --retry 3 "$progress" -o "$tag.tar.xz" "$base/$tag.tar.xz"
    sha512sum -c --quiet "$tag.sha512sum"
fi

rm -rf -- "./$tag"
tar -xf "$tag.tar.xz"
[ -f "$tag/compatibilitytool.vdf" ] && [ -f "$tag/version" ] && [ -f "$tag/proton" ] \
    || die "$tag.tar.xz does not contain a Proton tool named $tag"

# The tool's internal name and display name are the release's name; make them the fixed one.
pattern=$(printf '%s' "$tag" | sed 's/[][\.*^$/]/\\&/g')
replacement=$(printf '%s' "$name" | sed 's/[&/\]/\\&/g')
sed -i "s/\"$pattern\"/\"$replacement\"/g" "$tag/compatibilitytool.vdf"
[ "$(grep -c -F "\"$name\"" "$tag/compatibilitytool.vdf")" -eq 2 ] \
    || die "could not rename the tool in $tag/compatibilitytool.vdf"

cd "$dest"
if [ -e "$tool" ] || [ -L "$tool" ]; then
    old=$dest/$name.old
    rm -rf -- "$old"
    mv -- "$tool" "$old"
fi
mv -- "$staging/$tag" "$tool"
if [ -n "$old" ]; then
    rm -rf -- "$old"
fi
rm -rf -- "$staging"

echo "Installed $tag as \"$name\" in $dest"
if command -v pgrep >/dev/null && pgrep -x steam >/dev/null; then
    echo "Steam is running: restart it to see the change."
fi
echo "Pick \"$name\" in Steam under Settings > Compatibility, or per game under Properties > Compatibility."
echo "Run this script again to update it in place."
