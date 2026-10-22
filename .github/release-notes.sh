#!/bin/bash
# Print the release notes for a build: the Proton Experimental base and the patches applied.
# Usage: release-notes.sh <build name>
set -euo pipefail
name=$1
cd "$(dirname "$0")/.."

git fetch -q origin experimental_11.0 2>/dev/null || true
base=$(git merge-base HEAD origin/experimental_11.0 2>/dev/null || git rev-parse HEAD)
subject=$(git log -1 --format=%s "$base")
steam=$(sed -nE 's/^experimental-bleeding-edge-([0-9.]+)-[0-9]+-([0-9]{8})-.*/experimental-\1-\2/p' <<<"$subject")
wine_base=$(git ls-tree "$base" wine | awk '{print $3}')
wine_head=$(git ls-tree HEAD wine | awk '{print $3}')
repo=${GITHUB_REPOSITORY:-jegglest/proton-je}

steam_note=""
[ -n "$steam" ] && steam_note=" (Steam's \`$steam\`)"
echo "\`$name\` is [Proton Experimental](https://github.com/ValveSoftware/Proton/tree/experimental_11.0)$steam_note: Proton commit [\`${base:0:10}\`](https://github.com/ValveSoftware/Proton/commit/$base) with Wine [\`${wine_base:0:10}\`](https://github.com/ValveSoftware/wine/commit/$wine_base), built from Valve's sources in Valve's SDK container, with these commits on top of that Wine ([jegglest/wine](https://github.com/jegglest/wine/compare/$wine_base...$wine_head)) and, apart from the \`proton\` script's handling of the FSR 4 DLL, nothing else changed."
echo
# A release that needs a word of its own has it in docs-je/releases/<build name>.md.
if [ -f "docs-je/releases/$name.md" ]; then cat "docs-je/releases/$name.md"; echo; fi
# Commits by the repository's author are the ones meant for upstream Wine, except the game workarounds, which have
# HACK: in the subject as in Valve's Wine; everything else is carried from elsewhere.
upstream=() carried=() hacks=()
for commit in $(git -C wine rev-list --reverse "$wine_base..$wine_head"); do
    subject=$(git -C wine log -1 --format=%s "$commit") author=$(git -C wine log -1 --format=%an "$commit")
    line="- $subject ([\`${commit:0:10}\`](https://github.com/jegglest/wine/commit/$commit)), by $author"
    if [ "$author" != "Jason Eggleston" ]; then carried+=("$line")
    elif [[ $subject == *"HACK:"* ]]; then hacks+=("$line")
    else upstream+=("$line"); fi
done
echo "**For upstream Wine**, one page per patch in [\`docs-je/patches/\`](https://github.com/$repo/tree/$name/docs-je/patches): stale thread alerts left by \`RtlWaitOnAddress()\` (Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), the Space Marine 2 crash in Proton issue [#8072](https://github.com/ValveSoftware/Proton/issues/8072)) and ntsync mutexes that are never abandoned (Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417)):"
echo
printf '%s\n' "${upstream[@]}"
echo
echo "**Carried from Proton-EM, not for upstream**: Etaash Mathamsetty's FSR 4 driver-side work, as Proton-GE ships it, which makes the FSR 3.1 to FSR 4 upgrade work on RDNA4 cards (Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908)); the FSR 4 DLL itself comes from Steam's Proton Experimental install, see the [README](https://github.com/$repo/tree/$name#what-is-carried):"
echo
printf '%s\n' "${carried[@]}"
if [ ${#hacks[@]} -gt 0 ]; then
    echo
    echo "**Game workarounds**, in the form Valve's Wine uses for them, each on for one game only (write-ups in [\`docs-je/\`](https://github.com/$repo/tree/$name/docs-je)):"
    echo
    printf '%s\n' "${hacks[@]}"
fi
cat <<NOTES

AMD Anti-Lag 2 needs no patch, only \`ENABLE_LAYER_MESA_ANTI_LAG=1\` in the game's launch options (README). See [\`docs-je/\`](https://github.com/$repo/tree/$name/docs-je) for the write-up of each issue, with the test results on Linux and on Windows.

**Install:** extract \`$name.tar.xz\` into \`~/.steam/root/compatibilitytools.d/\` (for Flatpak Steam, \`~/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d/\`), restart Steam, then select \`$name\` under the game's Properties > Compatibility.

**Verify:** \`sha512sum -c $name.sha512sum\`
NOTES
