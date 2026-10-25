# Changelog fragments

Every pull request that changes something a user can observe adds one small
file here, instead of editing `CHANGELOG.md`. Editing the shared
`## [Unreleased]` section conflicts with every other open pull request; a new
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
- A name that already exists on `master` is rejected; pick another slug.

## Content

The file holds the entry exactly as it should read in `CHANGELOG.md`:

- the first line starts with `- `, continuation lines are indented two spaces;
  several bullets are fine;
- no blank lines, no headings (the category comes from the file name), no
  trailing whitespace, and exactly one newline at the end;
- nothing is rewrapped for you.

Write it for someone deciding whether to upgrade: what they saw before and
what they see now.

Example, `changelog.d/iis-event-log-warning.added.md`:

    - **IIS writes a warning when the optimizer service is unusable.** A process
      that starts without a usable service now logs one event-log entry.

## Do not

- Do not edit `## [Unreleased]` in `CHANGELOG.md` directly. (For now such an
  edit is still accepted by the CI check, but it will conflict.)
- Do not run the assembler in a feature branch. It runs in the release cut.

## Check and preview

    python3 -I tools/changelog/assemble.py --lint      # names and content
    python3 -I tools/changelog/assemble.py --preview   # the section as it would read

If you already edited `CHANGELOG.md` on your branch,
`python3 -I tools/changelog/to_fragment.py` moves the edit into fragments and
restores `CHANGELOG.md`; it commits nothing.
