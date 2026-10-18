---
name: mark-latest
description: Make a Proton-JE release that has been tried the one to use. Confirm it is GitHub's latest release, demote the earlier releases to pre-release so ProtonPlus offers only the newest, move the "Proton-JE Latest" tool in Steam to it with install-latest.sh, and point Steam's global default at that tool once, when Steam is closed. Use when asked to make a release the latest, promote it, mark older releases as pre-release, retire a release, update Proton-JE Latest, or set Steam's default Proton-JE.
---

# Mark a Proton-JE release as the latest

Argument: the release tag, default the newest one (`gh release list --repo jegglest/proton-je --limit 1`).
Building and installing a release is `/rebase-release`; this skill is the public "use this one" signal, given
after the build has been used in a game: ProtonPlus (provider definition in Vysp3r/ProtonPlus PR #1315) lists
only releases that are not pre-releases, the README points people at the newest release, and
`install-latest.sh` installs GitHub's latest release as the one Steam tool `Proton-JE Latest`.

## 1. Check the release

```bash
tag=proton-je-11.0-20261008-1
gh release view "$tag" --repo jegglest/proton-je --json isDraft,isPrerelease,assets \
    --jq '{isDraft,isPrerelease,assets:[.assets[].name]}'
gh release list --repo jegglest/proton-je                        # "Latest" marks GitHub's current latest release
cd ~/.local/share/Steam/compatibilitytools.d && cat "$tag/version" "Proton-JE Latest/version"   # "<epoch> <tag>"
```

Both assets (`<tag>.tar.xz`, `<tag>.sha512sum`) must be there and the release must not be a draft. If neither
installed copy is this release (`/rebase-release` installs it under its tag name for trying it side by side),
stop: it has not been tried.

## 2. GitHub: latest, and only it

```bash
gh release edit "$tag" --repo jegglest/proton-je --latest --prerelease=false
for old in $(gh release list --repo jegglest/proton-je --json tagName,isPrerelease \
        --jq '.[] | select(.isPrerelease | not) | .tagName' | grep -vx "$tag"); do
    gh release edit "$old" --repo jegglest/proton-je --prerelease
done
gh release list --repo jegglest/proton-je
```

The list must show `Latest` on `$tag` and `Pre-release` on every other tag. Keep an older release offered only
when the user says so (the new one breaks a game for them), and say which one stayed.

## 3. Move `Proton-JE Latest` to it

```bash
cd ~/repos/proton-je && ./install-latest.sh --tag "$tag"
cat ~/.local/share/Steam/compatibilitytools.d/"Proton-JE Latest"/version    # "<epoch> $tag"
```

The script downloads the release again, checks the sha512 sum, renames the tool to `Proton-JE Latest` in its
`compatibilitytool.vdf` and replaces the previous `Proton-JE Latest` directory; the name and path stay, so
every Steam mapping to that tool now runs this release. `--tag` makes it independent of what GitHub's
`releases/latest` redirect already says. With no argument it installs GitHub's latest, so without step 2 it
would reinstall the old one. It says "already installed" when the tool holds this release and exits 0, and
says when Steam is running and needs a restart to notice the change.

## 4. Steam's default

Proton-JE is Steam's global default compatibility tool, app `"0"` under `"CompatToolMapping"` in
`~/.local/share/Steam/config/config.vdf` (native Steam), not a per-game choice. Since 2026-10-17 it names
`Proton-JE Latest`, so step 3 is normally all there is: check and move on.

```bash
f=~/.local/share/Steam/config/config.vdf
grep -n '"name"[[:space:]]*"\(proton-je-\|Proton-JE \)' "$f"    # every mapping that names a Proton-JE build
```

If the default (the line inside the `"0"` block) names a `proton-je-<version>` build instead, point it at
`Proton-JE Latest` once, so later releases need no edit. Steam rewrites that file on exit, so edit it only
while Steam is closed; if `pgrep -x steam` finds it, ask the user to close Steam (or to pick `Proton-JE Latest`
in Steam > Settings > Compatibility) and leave this step for later.

```bash
cp "$f" "$f.before-$tag"
n=$(awk '/"CompatToolMapping"/{m=1} m && /^[[:space:]]*"0"$/{z=1} z && /"name"/{print NR; exit}' "$f")
sed -i -E "${n}s/(\"name\"[[:space:]]*)\"[^\"]*\"/\\1\"Proton-JE Latest\"/" "$f"
diff "$f.before-$tag" "$f"                             # exactly one line
```

Per-game mappings that still name an older `proton-je-*` build: list them for the user; change them only when
asked (the game's Properties > Compatibility does the same).

## 5. Afterwards

- `README.md`, the `Builds` row, says which release to use at minimum ("Use `proton-je-…` or later"): change
  it only when the new release fixes a defect of its predecessors, then commit and push `je-11.0`.
- Older installed tools in `~/.local/share/Steam/compatibilitytools.d/` stay, including the per-tag copy of
  this release; offer to remove the ones no mapping names any more, do not remove them unasked.
- Report: which tag is Latest, which tags were demoted, what `Proton-JE Latest` holds now, whether Steam's
  default needed a change (and the backup file's path if it did).
