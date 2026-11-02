# Saving the complete project to banderson777

The main publication target is:
https://github.com/banderson777/bpsec-bplib-integration

One parent repository presents the project. Modified upstream components retain
their history in forks. The parent records exact commits for each dependency.
A commit in the parent alone does not save uncommitted changes inside a submodule.

## Repositories included

| Source | Publication repository | Change ownership |
| --- | --- | --- |
| Integration, tests, demos, configuration and reviews | banderson777/bpsec-bplib-integration | Main project |
| BPLib | banderson777/bplib_and_BSL | Existing fork, codex/integration branch |
| BPNode | banderson777/bp | Fork, codex/integration branch |
| BSL | banderson777/BSL | Policy parser compatibility change |
| cFE | banderson777/cFE | Per-process RAM mount support for the native lab |
| cFS bundle | banderson777/cFS | Pins the modified cFE fork |

The existing bplib_and_BSL fork is retained. Its main branch need not be overwritten:
the integration branch preserves this working code separately. Other cFS and BSL
dependencies remain pinned to their upstream commits. The full source inventory
is in docs/publication/component-inventory-2026-11-02.json.

## Publication order

1. Review and commit modified component source, including BPNode's new demo/sink files.
2. Push cFE, BPLib, BPNode and BSL integration branches to the account's forks.
3. Commit/push cFS's updated cFE URL and commit pointer.
4. Commit the parent source, documentation, workflow, license and dependency pointers.
5. Push the parent to the new main repository and watch its CI build.
6. After a successful clean build, attach the complete-source ZIP to a public release.

GitHub browser authentication supplies credentials; passwords and access tokens
should not be pasted into chat or embedded in remote URLs. Author commits using
the account name and its GitHub no-reply address when a private email is preferred.

The publication excludes local account folders, virtual environments, build/staging
output, live database/WAL files, generated keys, recordings, segments and spools.
Those are generated again by the documented build/demo commands. Reviewed historical
validation logs remain with the code. LICENSE, NOTICE, THIRD_PARTY.md and component
license files preserve source attribution. DEPENDENCIES.lock.json records the
published dependency commits.

## Future saves

First commit/push changes inside each modified component. Then, in the parent,
commit/push the changed submodule pointers and any integration files. For example:

```bash
git -C bplib add -u
git -C bplib commit -m "Describe the BPLib changes"
git -C bplib push origin codex/integration

git add bplib integration client-test demo docs scripts .github README.md
git commit -m "Update integration and tested dependency commits"
git push origin main
```

Explicitly add any new source files in the component before committing; `add -u`
only stages already tracked files. Check `git status` in each changed repository
before committing. Never use a force push for ordinary saves.

## Downloading

Use a recursive clone as shown in [quick start](quickstart.md). For a release asset,
run `python3 scripts/package_source.py` from a committed, fully initialized checkout.
It exports committed Git objects, including recursive submodules, without Git
metadata or untracked runtime files. The included SOURCE_MANIFEST.json records
all component commits and file hashes. Public release assets are suitable for
users who prefer downloading a ZIP.
