---
name: rebase-release
description: Rebase Proton-JE onto the newest Proton Experimental, tag the release, build it on GitHub Actions, then download, verify and install it into Steam. Use when a new Proton Experimental is out, or when asked to update, rebase, re-tag, rebuild or install a new Proton-JE release.
---

# A new Proton-JE release on a new Proton Experimental

Layout: `~/repos/proton-je` is the Proton fork (branch `je-11.0`; remote `origin` = jegglest/proton-je over
SSH, `valve` = ValveSoftware/Proton). `wine/` is the submodule: a treeless clone of jegglest/wine (branch
`je-11.0`; remote `valve` = ValveSoftware/wine; both remotes fetch with the `tree:0` filter). The Proton tree
carries no patch files: the Wine commits on the fork are the patches, in two groups, first the author's own
fixes for upstream Wine, then the commits carried from Proton-EM. `.github/workflows/build.yml` builds any tag
`proton-je-*` and publishes `<tag>.tar.xz` + `<tag>.sha512sum` as a GitHub release; `.github/release-notes.sh`
writes the release body from the git history.

Do the steps in order. Every push goes to the public forks; stop and report when a step fails rather than
improvising. Nothing with personal data (play time, schedules) goes into docs or commit messages.

## 1. Find the new base

```bash
cd ~/repos/proton-je && git fetch valve experimental_11.0
old_base=$(git log -1 --format=%H --grep='^experimental-bleeding-edge-' HEAD)   # Valve's tip our branch sits on
new_base=$(git rev-parse valve/experimental_11.0)
git log -1 --format='%s' $new_base    # experimental-bleeding-edge-11.0-<build>-<YYYYMMDD>-p…-w<wine>-d…-v…
old_wine=$(git ls-tree $old_base wine | awk '{print $3}'); new_wine=$(git ls-tree $new_base wine | awk '{print $3}')
```

Same tip as before: nothing to rebase (a rebuild on the same base is only a new tag, `-<n+1>`, step 5).
Valve replaces its tip commit at every update (it only moves the submodule gitlinks), so `$old_base` is
normally not an ancestor of `$new_base`; `git log --oneline $old_base..$new_base` still shows what changed.
`git diff --stat $old_base $new_base -- proton Makefile.in configure.sh make/` previews conflicts with our
`proton` script change.

## 2. Rebase the Wine fork

```bash
cd wine && git fetch --filter=tree:0 valve experimental_11.0
git merge-base --is-ancestor $new_wine valve/experimental_11.0 || echo "gitlink not on the branch: fetch it by sha"
old_head=$(git rev-parse je-11.0)
git diff --name-only $old_wine $old_head > /tmp/ours.txt
git diff --name-only $old_wine $new_wine | grep -Fxf /tmp/ours.txt     # files Valve touched that we touch too
git rebase --onto $new_wine $old_wine je-11.0
```

Verify before anything is pushed: `git rev-list --count $new_wine..je-11.0` is the old count (31 at the time
of writing), and `git range-diff $old_wine..$old_head $new_wine..je-11.0` shows every commit as `=`. A
conflict means a patch needs a port: resolve it the way the upstream submission would, keep the message (and
the attribution line of an imported commit), and say what changed in the release page (step 4) and in the
range-diff summary of the docs commit.

Keep the old head reachable, because the published release notes link every commit of it: one annotated tag
per release built from it, named like that release (`git ls-remote --tags origin 'proton-je-*'` shows which
exist): `git tag -a <release tag> $old_head -m "Wine of Proton-JE release <release tag>"`.

Push, in this order (the mirror branch needs `--force-with-lease=experimental_11.0:<its old sha>` when Valve
rewrote it):

```bash
git push origin valve/experimental_11.0:refs/heads/experimental_11.0
git push origin <the new tags>
git push --force-with-lease=je-11.0:$old_head origin je-11.0
```

## 3. Rebase the Proton fork

The mirror branch first: on the runner, `release-notes.sh` takes the base from `merge-base HEAD
origin/experimental_11.0`, so the fork's `experimental_11.0` must be at `$new_base` or the notes name the wrong
base.

```bash
cd ~/repos/proton-je
git push --force-with-lease=experimental_11.0:$old_base origin valve/experimental_11.0:refs/heads/experimental_11.0
old_proton=$(git rev-parse je-11.0)
GIT_EDITOR=true git rebase --onto $new_base $old_base je-11.0
```

Every commit that moves the `wine` gitlink conflicts (4 at the time of writing). Resolve each with
`git update-index --cacheinfo 160000,<sha>,wine && GIT_EDITOR=true git rebase --continue`: the first one (the
sync fixes) gets the last author-owned commit of the rebased Wine series,
`git -C wine log -1 --format=%H --author='Jason Eggleston' $new_wine..je-11.0`; every later one gets the new
Wine head, `git -C wine rev-parse je-11.0`. Any other conflict is Valve changing a file we change (the `proton`
script): keep Valve's change and ours.

Verify: `diff <(git diff $old_base $old_proton -- . ':!wine') <(git diff $new_base HEAD -- . ':!wine')` shows
only `index`/`@@` offset lines; `git ls-tree HEAD wine` is the new Wine head; `git submodule status` shows
`wine` without a `+`.

## 4. Docs

The docs name the base and link the upstream patches by commit id, so a rebase changes them. Map old ids to new
ones with `paste <(git -C wine rev-list --reverse $old_wine..$old_head) <(git -C wine rev-list --reverse
$new_wine..je-11.0)` and update, with `sed`:

- `README.md`, the `Base` row: Proton commit and link, Steam build name (`experimental-11.0-<YYYYMMDD>`), Wine
  commit and link.
- The compare links `<old_wine, 11 chars>...<old upstream end, 11 chars>` in `README.md` and
  `docs-je/README.md`, including the `.patch` one and the `git format-patch` range in the text.
- The upstream commits, 40- and 10-character forms, in `docs-je/README.md` and `docs-je/patches/*.md`.
- The `Applies to` rows of `docs-je/patches/*.md`: add the new Valve Wine commit (with its link); the dated
  "Checked on" sentence in `docs-je/README.md`.

Leave history alone: results quoted on older builds, `docs-je/releases/`, `ntsync-mutex-abandonment.md`'s
short id of the commit a test was built from. Afterwards `grep -rn '<old ids>' README.md docs-je` hits only
those. A release that needs a word of its own (a conflict resolved, a fix) gets `docs-je/releases/<tag>.md`,
which `release-notes.sh` prints into the release body.

Commit as `Rebase onto Proton Experimental <YYYYMMDD>`: new Proton and Wine commits, that the Wine commits are
unchanged as patches (or what changed), the gitlink, the docs, the tags left on the fork. Then
`git push --force-with-lease=je-11.0:$old_proton origin je-11.0`.

## 5. Tag and build

Tag name `proton-je-<major.minor>-<YYYYMMDD>-<n>` from Valve's subject (`…-11.0-449079-20261008-…` gives
`proton-je-11.0-20261008-1`); `-<n>` counts builds on the same base. Annotated, message = the tag name, a blank
line, one paragraph on what it is (base, same patches as the previous release or what is new).

```bash
git tag -a $tag -F msg.txt && git push origin $tag
gh run list --limit 1            # the Build workflow for the tag
gh run watch <run id> --exit-status --interval 60   # in the background; 25-30 min with a warm ccache, 2 h cold
```

A failed run publishes nothing: `gh run view <id> --log-failed`, fix on the branch, delete the tag locally and
on `origin`, re-tag the same name on the fix. A build to try without a release is a `workflow_dispatch`
(`gh workflow run build.yml`), which uploads an artifact named `proton-je-dev-<sha>`.

## 6. Download, verify, install

```bash
d=~/Downloads/$tag && mkdir -p "$d" && cd "$d"
gh release download "$tag" --repo jegglest/proton-je -D .
sha512sum -c "$tag.sha512sum" && tar -xf "$tag.tar.xz" && cat "$tag/version"   # "<epoch> <tag>"
```

Functional check with the probes of `docs-je/repro/` (sources there; `mutex_abandon.exe`,
`raced_wake_probe.exe` and their `32` builds live in `~/repos/sm2/stale-alert/repro/`; build the others with
MinGW in the podman image `je-wine-build`), with the tool's own Wine and a scratch prefix, ntsync on:

```bash
T=$d/$tag; export WINEPREFIX=/tmp/je-test LD_LIBRARY_PATH=$T/files/lib64:$T/files/lib WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all
$T/files/bin/wine mutex_abandon.exe close 3; $T/files/bin/wine mutex_abandon.exe waitall 3   # 0 failed steps, "abandoned" lines
$T/files/bin/wine inproc_cache_probe.exe 5000                                                 # "waited on all 5000 events: OK"
$T/files/bin/wine raced_wake_probe.exe 20000 3                                                # 0 stale alerts, 0 early returns
$T/files/bin/wineserver -k
```

The 32-bit builds matter for the mutex and cache probes (the 32-bit unix `ntdll.so` is separate). Expected
output is spelled out in `docs-je/repro/README.md`.

Install: `mv "$d/$tag" ~/.local/share/Steam/compatibilitytools.d/` (native Steam). If Steam is running
(`pgrep -x steam`) it lists the tool after a restart. Do not touch `config.vdf` here: making the release
Steam's default and GitHub's only non-prerelease is `/mark-latest`, once the build has been used.

## Report

Base before and after (Proton and Wine commits, Steam build names), whether the patches changed, the tag, the
run id and duration, the sha512 result, the probe results, where the tool is installed, and whether Steam needs a
restart. When Valve moves to `experimental_12.0`, start `je-12.0` on both forks the same way and update the
branch names in `.github/release-notes.sh` and here.
