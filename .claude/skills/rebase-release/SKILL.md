---
name: rebase-release
description: Rebase Proton-JE onto the newest Proton Experimental, tag the release, build it on GitHub Actions, then download, verify and install it into Steam. Use when a new Proton Experimental is out, or when asked to update, rebase, re-tag, rebuild or install a new Proton-JE release.
---

# A new Proton-JE release on a new Proton Experimental

Layout: `~/repos/proton-je` is the Proton fork (branch `je-11.0`; remote `origin` = jegglest/proton-je over SSH,
`valve` = ValveSoftware/Proton). `wine/` is the submodule: a full clone of jegglest/wine (branch `je-11.0`; remote
`valve` = ValveSoftware/wine), never a shallow or partial one. The Proton tree carries no patch files: the Wine
commits on the fork are the patches, in three groups, first the author's own fixes for upstream Wine, then the
commits carried from Proton-EM, then fixes taken early from Valve's own newer Wine (commit 32, Valve's
`RtlpUnWaitCriticalSection()` fix for the Helldivers 2 crash, from its bleeding-edge branch).
`.github/workflows/build.yml` builds any tag `proton-je-*` and publishes `<tag>.tar.xz` + `<tag>.sha512sum` as a
GitHub release; `.github/release-notes.sh` writes the release body from the git history.

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
cd wine && git fetch valve experimental_11.0 bleeding-edge
git merge-base --is-ancestor $new_wine valve/experimental_11.0 || echo "gitlink not on the branch: fetch it by sha"
git log --oneline $new_wine -F --grep='Ignore semaphore errors in RtlpUnWaitCriticalSection()'   # Valve's fix arrived?
old_head=$(git rev-parse je-11.0)
git diff --name-only $old_wine $old_head > /tmp/ours.txt
git diff --name-only $old_wine $new_wine | grep -Fxf /tmp/ours.txt     # files Valve touched that we touch too
git rebase --onto $new_wine $old_wine je-11.0
```

Verify before anything is pushed: `git rev-list --count $new_wine..je-11.0` is the old count (32 at the time
of writing), and `git range-diff $old_wine..$old_head $new_wine..je-11.0` shows every commit as `=`. Once Valve's
Wine for the new Experimental has a commit taken early (the `grep` above prints it), the rebase drops ours as
empty and the count is one lower: take its tag out of the loops below, and its rows out of the READMEs and the
write-up's header in step 4, saying in the docs commit that Proton Experimental now has the fix. A
conflict means a patch needs a port: resolve it the way the upstream submission would, keep the message (and
the attribution line of an imported commit), and say what changed in the release page (step 4) and in the
range-diff summary of the docs commit.

The docs link the five upstream patches and Valve's fix through tags on the fork, and their compare links start from
the fork's `experimental_11.0`, so a rebase moves the tags instead of editing commit ids. Each tag's new commit is
the one with the same subject in the rebased series:

```bash
for t in waitonaddress-use-after-return waitonaddress-stale-alert waitonaddress-stale-alert-test \
         ntsync-mutex-owned-after-close ntsync-mutex-in-use-after-close cs-unwait-ignore-errors; do
  git tag -f $t $(git log -1 --format=%H -F --grep="$(git log -1 --format=%s $t)" $new_wine..je-11.0)
done
git rev-list --count $new_wine..ntsync-mutex-in-use-after-close    # 5: the tags sit on the first five commits
git rev-parse cs-unwait-ignore-errors je-11.0                       # the same twice: Valve's fix is last
```

Keep the old head reachable, because the published release notes link every commit of it: one annotated tag
per release built from it, named like that release (`git ls-remote --tags origin 'proton-je-*'` shows which
exist): `git tag -a <release tag> $old_head -m "Wine of Proton-JE release <release tag>"`.

Push, in this order (the mirror branch needs `--force-with-lease=experimental_11.0:<its old sha>` when Valve
rewrote it; the patch tags moved, so they go with `--force`, and a clone that fetched them before has to fetch
them with `--force` too):

```bash
git push origin valve/experimental_11.0:refs/heads/experimental_11.0
git push origin <the new release tags>
git push --force-with-lease=je-11.0:$old_head origin je-11.0
git push --force origin refs/tags/waitonaddress-use-after-return refs/tags/waitonaddress-stale-alert \
    refs/tags/waitonaddress-stale-alert-test refs/tags/ntsync-mutex-owned-after-close \
    refs/tags/ntsync-mutex-in-use-after-close refs/tags/cs-unwait-ignore-errors
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

If the first pick dies with `fatal: invalid commit position. commit-graph is likely corrupt` (git 2.56 on
2026-10-16, with `git commit-graph verify` clean in both repositories), `git rebase --abort` and run the rebase,
and every `--continue` below, as `git -c core.commitGraph=false rebase …`.

Every commit that moves the `wine` gitlink conflicts (2 on 2026-10-16, 3 since the Helldivers 2 fix).
Resolve each with `git update-index --cacheinfo 160000,<sha>,wine && GIT_EDITOR=true git rebase --continue`,
taking the commits from the tags moved in step 2: the first one (the sync fixes) gets
`git -C wine rev-parse ntsync-mutex-in-use-after-close`; the second (the FSR 4 commits) gets the commit before
Valve's fix, `git -C wine rev-parse cs-unwait-ignore-errors~1`; the last one (Valve's fix for Helldivers 2) gets
the new Wine head, `git -C wine rev-parse je-11.0`. Take them from the tags rather than from authors: commit 32
is neither the author's nor Proton-EM's. Any other conflict is Valve changing a file we change (the
`proton` script): keep Valve's change and ours.

Verify: `diff <(git diff $old_base $old_proton -- . ':!wine') <(git diff $new_base HEAD -- . ':!wine')` shows
only `index`/`@@` offset lines; `git ls-tree HEAD wine` is the new Wine head; `git submodule status` shows
`wine` without a `+`.

## 4. Docs

The docs name the base and record which Valve Wine the patches were checked against; the patch links are the
tags moved in step 2 and need nothing. Update, with `sed`:

- `README.md`, the `Base` row: Proton commit and link, Steam build name (`experimental-11.0-<YYYYMMDD>`), Wine
  commit and link.
- The `Applies to` rows of `docs-je/patches/*.md`: add the new Valve Wine commit (with its link); the dated
  "Checked on" sentence in `docs-je/README.md`.

Leave history alone: results quoted on older builds, `docs-je/releases/`, `ntsync-mutex-abandonment.md`'s
short id of the commit a test was built from. Afterwards `grep -rn '<old base ids>' README.md docs-je` hits only
those and the `Applies to` and "Checked on" lines. A release that needs a word of its own (a conflict resolved, a
fix) gets `docs-je/releases/<tag>.md`, which `release-notes.sh` prints into the release body.

Commit as `Rebase onto Proton Experimental <YYYYMMDD>`: new Proton and Wine commits, that the Wine commits are
unchanged as patches (or what changed), the gitlink, the docs, the patch tags moved and the release tags left on
the fork. Then `git push --force-with-lease=je-11.0:$old_proton origin je-11.0`.

## 5. Tag and build

Tag name `proton-je-<major.minor>-<YYYYMMDD>-<n>` from Valve's subject (`…-11.0-449079-20261008-…` gives
`proton-je-11.0-20261008-1`); `-<n>` counts builds on the same base. Annotated, message = the tag name, a blank
line, one paragraph on what it is (base, same patches as the previous release or what is new).

```bash
git tag -a $tag -F msg.txt && git push origin $tag
gh run list --limit 1            # the Build workflow for the tag
gh run watch <run id> --exit-status --interval 60   # in the background; 25-45 min with a warm ccache (the Build step alone about 30), 2 h cold
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
$T/files/bin/wine cs_leave_bad_semaphore.exe                                                    # "returned" in all three modes
$T/files/bin/wineserver -k
```

The 32-bit builds matter for the mutex and cache probes (the 32-bit unix `ntdll.so` is separate). Expected
output is spelled out in `docs-je/repro/README.md`.

Install: `mv "$d/$tag" ~/.local/share/Steam/compatibilitytools.d/` (native Steam). If Steam is running
(`pgrep -x steam`) it lists the tool after a restart. Do not run `install-latest.sh` or touch `config.vdf`
here: the `Proton-JE Latest` tool, Steam's default, follows GitHub's latest release and moves in
`/mark-latest`, once the build has been used side by side under its tag name.

## Report

Base before and after (Proton and Wine commits, Steam build names), whether the patches changed, the tag, the
run id and duration, the sha512 result, the probe results, where the tool is installed, and whether Steam needs a
restart. When Valve moves to `experimental_12.0`, start `je-12.0` on both forks the same way and update the
branch names in `.github/release-notes.sh` and here.
