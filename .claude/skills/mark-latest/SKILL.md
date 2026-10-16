---
name: mark-latest
description: Make a Proton-JE release that has been tried the one to use. Confirm it is GitHub's latest release, demote the earlier releases to pre-release so ProtonPlus offers only the newest, and switch Steam's global default to it when Steam is closed. Use when asked to make a release the latest, promote it, mark older releases as pre-release, retire a release, or set Steam's default Proton-JE.
---

# Mark a Proton-JE release as the latest

Argument: the release tag, default the newest one (`gh release list --repo jegglest/proton-je --limit 1`).
Building and installing a release is `/rebase-release`; this skill is the public "use this one" signal, given
after the build has been used in a game: ProtonPlus (provider definition in Vysp3r/ProtonPlus PR #1315) lists
only releases that are not pre-releases, and the README points people at the newest release.

## 1. Check the release

```bash
tag=proton-je-11.0-20261008-1
gh release view "$tag" --repo jegglest/proton-je --json isDraft,isPrerelease,assets \
    --jq '{isDraft,isPrerelease,assets:[.assets[].name]}'
gh release list --repo jegglest/proton-je                        # "Latest" marks GitHub's current latest release
cat ~/.local/share/Steam/compatibilitytools.d/$tag/version      # the installed copy, "<epoch> <tag>"
```

Both assets (`<tag>.tar.xz`, `<tag>.sha512sum`) must be there and the release must not be a draft. If it is
not installed, stop: it has not been tried.

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

## 3. Steam's default

Proton-JE is Steam's global default compatibility tool, app `"0"` under `"CompatToolMapping"` in
`~/.local/share/Steam/config/config.vdf` (native Steam), not a per-game choice. Steam rewrites that file on
exit, so edit it only while Steam is closed; if `pgrep -x steam` finds it, ask the user to close Steam (or
to pick the tool in Steam > Settings > Compatibility) and leave this step for later.

```bash
f=~/.local/share/Steam/config/config.vdf && cp "$f" "$f.before-$tag"
grep -n '"name"[[:space:]]*"proton-je-' "$f"          # every mapping that names a Proton-JE build
n=$(awk '/"CompatToolMapping"/{m=1} m && /^[[:space:]]*"0"$/{z=1} z && /"name"/{print NR; exit}' "$f")
sed -i -E "${n}s/\"proton-je-[^\"]*\"/\"$tag\"/" "$f"
diff "$f.before-$tag" "$f"                             # exactly one line
```

Per-game mappings that still name an older `proton-je-*` build: list them for the user; change them only when
asked (the game's Properties > Compatibility does the same).

## 4. Afterwards

- `README.md`, the `Builds` row, says which release to use at minimum ("Use `proton-je-…` or later"): change
  it only when the new release fixes a defect of its predecessors, then commit and push `je-11.0`.
- Older installed tools in `~/.local/share/Steam/compatibilitytools.d/` stay; offer to remove the ones no
  mapping names any more, do not remove them unasked.
- Report: which tag is Latest, which tags were demoted, whether Steam's default moved (or why not), and the
  backup file's path.
