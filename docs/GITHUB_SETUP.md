# Putting this project on GitHub

A guide for the collaborators (Tnes and Gabriel B / `g-brrzzn`), and for any coding agent working for either of
them. Use it whoever sets the repository up, and from whatever you start with: this folder with its Git
history, the zip, or a repository that already exists.

Nothing here has been done yet. Creating an account, organisation or repository, pushing, and making anything public
are actions a person takes; a coding agent prepares commands and does not run them on its own.

## What you have

- **The zip** (`lswtcs-*.zip`): every tracked file at the latest commit, without Git history. It also carries
  `_history/lswtcs-history.bundle`.
- **The history bundle** (`lswtcs-history.bundle`): the full Git history of this work:
  - the first imports the 1 November 2026 handoff snapshot;
  - the rest are Gabriel's work since then, authored `g-brrzzn <136928835+g-brrzzn@users.noreply.github.com>`.

  A GitHub noreply address links those commits to Gabriel's GitHub profile without exposing a personal email.
- **Gabriel's own folder:** the same history in a normal Git repository (branch `diag/run-logs`).

## Never commit or push

- Game files, disc images, `default.xex`, movies, audio or anything extracted from the game.
- Generated guest code (`source/Convert 360/LSWTCS/output*`), build folders (`source/switch/build*`), objects, `.elf`,
  `.nro` and `.nacp` files.
- Keys (`prod.keys`, `title.keys`), saves, memory or GPU captures, shader packs.
- `private/`: logs, captures, PGO counts and streams, staged SD files, personal notes. PGO counts are derived from
  running the game.
- Personal paths, real names or emails you do not mean to publish. Before the first push, set the identity you want
  for new commits:
  ```sh
  git config user.name "<name or handle>"
  git config user.email "<id>+<user>@users.noreply.github.com"   # GitHub, Settings, Emails
  ```

`.gitignore` already excludes all of the above. Still run `git status` before every commit and push, and look at
what is listed.

## Decide before the repository is public

- **Organisation and visibility.** A GitHub organisation owned by both collaborators avoids the repository belonging to one
  person. Start it **private**, and make it public only when both agree.
- **Licence.** The port's own code keeps the original port's CC BY-NC-SA 4.0 (`LICENSE`, `notices/README.md`). Bundled third-party code keeps its own
  licences and notices: FFmpeg, SIMDe, SPIRV-Cross, XenonRecomp, xenia-canary, xxHash, rpmalloc. Do not put one
  blanket licence over the whole tree.

## Case 1: Gabriel sets it up and pushes this history (simplest)

1. Create the organisation and an empty **private** repository in it. Add no README or licence, so the push is not
   rejected. Invite the other collaborator as an organisation owner.
2. In Gabriel's folder:
   ```sh
   git status                      # must be clean
   git branch -m diag/run-logs main
   git remote add origin <repository URL>
   git push -u origin main
   ```
3. The other collaborator then clones it (`git clone <repository URL>`) and works from there. Each person's commits carry their
   own identity.

## Case 2: the other collaborator sets it up from the zip, keeping the history

1. Create the organisation and an empty private repository, and invite Gabriel.
2. Unpack the zip. In the folder that holds `_history/lswtcs-history.bundle`:
   ```sh
   git clone -b diag/run-logs _history/lswtcs-history.bundle lswtcs
   cd lswtcs
   git branch -m main
   git remote add origin <repository URL>
   git push -u origin main
   ```
3. Check that the cloned folder matches the zip: the bundle's last commit is the one the zip was made from.

## Case 3: an existing repository with its own history

If its main branch's content is the 1 November snapshot, replay Gabriel's commits on top of it and skip the import
commit:

```sh
git fetch <path>/lswtcs-history.bundle diag/run-logs:gabriel-work
git log --reverse --format=%h gabriel-work | head -1    # the import commit; call it BASE
git rebase --onto main BASE gabriel-work
git checkout main
git merge --ff-only gabriel-work
git push
```

If its main branch has moved on, resolve conflicts during the rebase file by file. The largest changed files are
`source/switch/source/gl_backend.cpp` and `source/LSWTCSRuntime/source/kernel_stubs.cpp`.

## Case 4: the zip's files without any history

This works, but every file then arrives in one commit by whoever makes it, and Gabriel's work is not credited per
commit. If you go this way, add this line at the end of that commit's message:

```
Co-authored-by: g-brrzzn <136928835+g-brrzzn@users.noreply.github.com>
```

Keep the credit line in `README.md` too.

## Keeping the credit

- **Do not squash** Gabriel's commits into one when merging or pushing. Squashing replaces their authorship. If they
  must be squashed, add the `Co-authored-by` line above.
- **On GitHub's contribution graph,** commits count once they are on the repository's default branch and the author
  is linked to the repository: a collaborator or organisation member, or someone who has starred it, forked it, or
  opened an issue or pull request in it.
- **A private repository** shows its contributions only to people with access, unless each person enables "private
  contributions" on their profile.

## After setup

- Work on branches, and merge into `main` through pull requests when both want to review.
- Logs and test results stay out of the repository. Findings go into `docs/progress-log.md` and
  `docs/reviews/CPU_ROADMAP.md`, as so far.
- Read `README.md` first, then `docs/reviews/CPU_ROADMAP.md` (its last section is the current state), then
  `docs/BUILDING.md` for the build. `AGENTS.md` holds the rules for coding agents.
