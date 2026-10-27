# How changes get into master

`master` is what everyone builds from, so nothing reaches it except through a reviewed pull
request. GitHub enforces this for everyone, the maintainer included: no direct pushes, no force
pushes, and the branch can't be deleted.

## The process

1. **Branch from `master`**, one branch per piece of work, named for it (`linux-build`,
   `vulkan-m1`). Contributors without write access fork the repository and branch there.
2. **Commit on the branch.** Keep commits focused, with messages that say what changed and why.
   Never commit game material: no game files, executables, archives, generated translations of the
   game's code, logs or captures (see [Legal](README.md#legal)). Run `python -I tools/audit_repo.py`
   before pushing.
3. **Open a pull request into `master`.** Its description says what changed, what was built
   (which programs: front end, campaign, Instant Action; Windows or Linux), and what was tested in
   play and what wasn't.
4. **Before it can merge:**
   - the `audit` check passes (`.github/workflows/audit.yml`, the same script);
   - the branch is up to date with `master`;
   - every review conversation is resolved. CodeRabbit reviews each pull request. Fix what holds
     up; answer what doesn't with the reason; then resolve the thread.
5. **Merge with a merge commit** (the "Create a merge commit" button), so a pull request's
   history stays together in `master`.

## Releases

Releases are tagged on `master` once the pull requests that make them are merged. **They are
source only:** never attach builds, game files or private test packages (Steam Deck zips among
them). Players build from their own disc ([docs/SETUP.md](docs/SETUP.md)).
