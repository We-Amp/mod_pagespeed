# Changelog fragments

Every pull request that changes something a user can observe adds one small
file here, instead of editing `CHANGELOG.md`. Editing the shared
`## Unreleased` section conflicts with every other open pull request; a new
file never does. The release cut folds the fragments into `CHANGELOG.md` and
deletes them.

## Name

    changelog.d/<slug>.<category>.md

- `<slug>` is your branch name with `/` turned into `-` (lowercase letters,
  digits and hyphens, 3 to 63 characters). Use one slug for the whole pull
  request.
- `<category>` is one of: `security`, `added`, `changed`, `deprecated`,
  `fixed`, `removed`, `documentation`, `upgrade-notes`, `known-issues`.
- At most one fragment per category. If a change needs two (a security fix
  that also changes a default), use the same slug with two categories.
- Security entries (a `*.security.md` fragment, or a Security line added to
  `CHANGELOG.md`) land only together with the release that ships the fix: the
  release-note gate refuses them unless the pull request is a
  `landing/<version>` branch with `Release-Note: landing -- <why>`, or says
  `Release-Note: security-public -- <why>`. Security fixes are not opened as
  public pull requests; follow `SECURITY.md`.

## Content

This file's `CHANGELOG.md` uses prose paragraphs led by the category, so a
fragment is one paragraph whose first words are the category name:

    Changed: the worker's `--help` output now lists the `--rsl-cap-*` options.

- the first line starts with the category's label, which is the label the
  history already uses: `Security: `, `Added: `, `Changed: `, `Deprecated: `,
  `Fixed: `, `Removed: `, `Documentation: `, `Upgrade: ` (file category
  `upgrade-notes`) or `Known issue: ` (file category `known-issues`);
- no blank lines, no headings, no trailing whitespace, exactly one newline at
  the end;
- nothing is rewrapped for you.

Write it for someone deciding whether to upgrade: what they saw before and
what they see now.

## Security entries and the CI check

CI checks every pull request that changes `src/` or `lib/`:

- only a fragment the pull request adds or modifies counts as the note; deleting
  one does not;
- the added lines of every fragment are scanned for a Security entry, so one
  hidden in a `*.fixed.md` is treated like a `*.security.md`;
- `changelog.d/` holds only `README.md` and `<slug>.<category>.md` files; any
  other file, or a subdirectory, fails `assemble.py --lint`.

## Converting a pull request that already edits CHANGELOG.md

`to_fragment.py` restores `CHANGELOG.md` to the merge-base, so run it BEFORE
rebasing onto the `main` that carries this directory:

1. Commit your work. The tool finds the repository from its own location, so
   a branch without it gets it in place, untracked: `mkdir -p tools/changelog
   && for f in assemble to_fragment; do git show origin/main:tools/changelog/$f.py
   > tools/changelog/$f.py; done`. Then run
   `python3 -I tools/changelog/to_fragment.py`. It exits 2 when the edit
   removes lines or touches text outside `## Unreleased`; then convert by
   hand.
2. `rm -r tools/changelog` if you added it (a leftover untracked copy blocks
   the rebase), `git add changelog.d && git commit`, then
   `git rebase origin/main`.
3. `python3 -I tools/changelog/assemble.py --lint`, push once.

Already rebased and stuck on a `CHANGELOG.md` conflict? Abort the rebase, run
step 1 on the pre-rebase branch, then rebase. RELEASING.md has the release side.

## Do not

- Do not edit `## Unreleased` in `CHANGELOG.md` directly. (The CI gate still
  accepts such an edit during the transition, but it will conflict.)
- Do not run the assembler in a feature branch. It runs in the release cut.

## Check and preview

    python3 -I tools/changelog/assemble.py --lint      # names and content
    python3 -I tools/changelog/assemble.py --preview   # the section as it would read

If you already edited `CHANGELOG.md` on your branch,
`python3 -I tools/changelog/to_fragment.py` moves the edit into fragments and
restores `CHANGELOG.md`; it commits nothing.
