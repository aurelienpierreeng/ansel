# image_test.sh

`tests/image_test.sh` runs `ansel-cli` over a local folder of camera raw files to catch
crashes, hangs, and (optionally) pixel-level regressions before you commit pipeline
changes, without needing a hand-curated reference checked in per camera or operation:
point it at whatever raws you have lying around (ideally a mix of Bayer, X-Trans, and a
few DNGs) and it validates that each one still exports.

Run results are **not** part of this repository (see `.gitignore` in this directory) -- but
the raws, and the shared regression baseline, live in a **shared team bank**: the private
submodule at `tests/image_test/samples/` (https://github.com/aurelienpierreeng/ansel-test,
raws via Git LFS, baseline PNGs committed directly since they're small).

## Setup (once)

```sh
git submodule update --init --checkout tests/image_test/samples
```

That's it -- `tests/image_test.sh` uses this shared bank automatically once it's checked out
(needs `git-lfs` installed: `sudo apt install git-lfs && git lfs install`, once per machine).

This submodule is registered with `update = none` and `active = false` in `.gitmodules`, so
it's opt-in: a plain `git submodule update --init --recursive` (the common blanket setup
command, also used by CI) or `git clone --recurse-submodules`/`git pull --recurse-submodules`
skips it silently instead of trying to pull this private repo. Fetching it always requires the
explicit `--checkout` invocation above.

Once you've opted in this way, it stays checked out, and a later plain `git pull
--recurse-submodules` *will* fast-forward it to whatever commit the superproject has pinned
-- that's intentional, it's how you pick up shared baseline updates. To sync it right away
instead of waiting for your next pull, run the same command as initial setup minus `--init`
(already registered, so not needed, but harmless to keep):

```sh
git submodule update --checkout tests/image_test/samples
```

If you want your local checkout to stop tracking it even after opting in, override it
per-machine with `git config submodule.tests/image_test/samples.active false` (not committed
-- this only applies to your own clone).

To use your own raws instead (or on top of it), point at any local folder:

```sh
tests/image_test.sh configure /path/to/your/raw/folder   # remembered in bank_path.conf (gitignored)
tests/image_test.sh --bank /path/to/your/raw/folder      # or one-off, via $ANSEL_IMAGE_TEST too
```

An explicit `--bank`/`configure`/`$ANSEL_IMAGE_TEST` always takes priority over the shared
submodule bank.

Raws are found recursively by extension (cr2, cr3, nef, arw, raf, rw2, dng, ...). Drop a
`<file>.xmp` sidecar next to a raw (same convention as darkroom/`ansel-cli`) to exercise a
specific module/history stack instead of the default pipeline.

### Contributing raws to the shared bank

The bank repo is private but shared, so treat it as such: only add raws you have the rights
to share with the team (your own shots, or samples licensed for it -- no personal photos
that aren't meant to be redistributed). See its own README for the contribution steps
(`git lfs track` any new extension before committing it, `<Make>/<Model>/` layout). GitHub's
free LFS quota is 10 GiB storage + 10 GiB bandwidth/month pooled on the owning account, and
every `git submodule update` a teammate runs spends bandwidth against it -- keep the bank
reasonably sized, and don't wire it into CI (this is a local/dev-only tool by design).

## Usage

```sh
tests/image_test.sh              # run the whole bank
tests/image_test.sh --limit 10   # quick pass over the first 10 raws
tests/image_test.sh --keep       # keep outputs/logs under tests/image_test/results/ even on success
```

A raw is a **failure** if `ansel-cli` crashes, hangs past `--timeout` (default 180s), or
produces an empty/missing output. Logs for every image are written to
`tests/image_test/results/<relative-path>.log`; on success the whole `results/` dir is
removed unless `--keep` is given or there was a failure.

## CPU vs. GPU

```sh
tests/image_test.sh --opencl
```

Also renders each raw a second time with OpenCL enabled and reports the delta-E between the
CPU and GPU output. Never fails just because OpenCL is unavailable (no GPU, no runtime) --
but fails if more than 5% of pixels exceed the delta-E tolerance, since that's a real,
widespread CPU/GPU divergence rather than the odd edge/border pixel. A high max dE with a
low percentage is not by itself a failure. `--strict-opencl` tightens that 5% share to 0%:
any single pixel over tolerance fails.

## Regressions vs. a baseline

```sh
tests/image_test.sh update-baseline   # snapshot current outputs as the reference
tests/image_test.sh                   # subsequent runs report any delta-E vs. baseline
tests/image_test.sh --strict-cpu      # zero-tolerance: fail on any drift, even invisible to the eye
```

When using the shared team bank, the baseline lives at `tests/image_test/samples/baseline/`
-- committed in the `ansel-test` submodule and visually reviewed before being pushed, so
every dev compares against the exact same reference. `update-baseline` always exports at a
fixed **1024x1024** for this baseline (`--width`/`--height` are ignored, with a warning) --
comparing at any other size is a guaranteed size-mismatch failure, not a real regression, so
don't override them for a `run` you intend to compare against it either. A personal/local bank
(`--bank`/`configure`) gets its own local, gitignored `tests/image_test/baseline/` instead, at
whatever size you export at.

Comparison against the baseline uses a CIE 2000 delta-E check in Lab space
(`tests/image_test/deltae`). By default, a change only fails the test if it's the kind a
human would actually notice: `max dE > 2.3` (or `avg dE > 0.77`) is a **failure**
("visually changed"); a smaller, technically-nonzero dE is reported but does not fail, and
`max dE < 0.01` is a perfect match. `--strict-cpu` removes that tolerance entirely -- any
nonzero dE fails, useful when you want to catch the smallest unintended pixel drift from a
code change, not just an obviously visible regression. A size mismatch against the baseline
is always a failure either way.

`--strict` is shorthand for `--strict-cpu --strict-opencl` together.

Each note (baseline and `--opencl`) also shows the **% of pixels above the 2.3 tolerance**,
alongside max/avg dE. A high max dE with ~0% of pixels above tolerance means a handful of
outlier pixels (typically an edge/border effect from a geometry-transforming module), not a
real widespread visual change -- check the average and that percentage before treating a high
max dE as a real regression. The full distribution (`deltae`'s complete report: max/avg/std dE
and the percentage of pixels within N standard deviations) is saved to
`tests/image_test/results/<relative-path>.deltae.log` (and `.cl.deltae.log` for `--opencl`)
for every comparison, not just failures.

`update-baseline` only **adds missing entries** -- it never overwrites or deletes an existing
one, and only runs `ansel-cli` on the raws that actually need a new entry (not the whole bank).
To force-refresh a specific entry after a deliberate pipeline/rendering change, delete its PNG
from the baseline first, then re-run `update-baseline`; visually check the new output before
committing/pushing it to the shared baseline. A crashing run never touches the baseline at all.

## Before committing

The script never runs automatically unless you've configured a bank -- see
`.githooks/pre-commit`, which calls
`tests/image_test.sh --if-configured --quiet --keep --opencl`
so an unconfigured checkout is unaffected.

Options, `run`/`configure`/`update-baseline` are documented in the header of
`tests/image_test.sh` (`tests/image_test.sh --help`).
