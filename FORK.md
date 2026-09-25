# Stelaxis Cloud Spanner Emulator fork

Stelaxis tests need concurrent read-write transactions and persistence, which
upstream does not provide. This fork is for development and tests only, not
production. Phase 0 establishes CI; later phases add those capabilities. Upstream
does not accept contributions.

## Updating from upstream

`master` is protected: PRs and merge commits are required, with these exact checks:

- `Build (amd64)`
- `Build (arm64)`
- `Upstream unit and conformance tests`

**Upstream-merge PRs MUST use a merge commit. Never squash or rebase them, even if
repository-wide settings offer those options: doing so loses upstream ancestry.**

Merge each upstream release into the fork:

```sh
git remote add upstream https://github.com/GoogleCloudPlatform/cloud-spanner-emulator.git # once
git fetch origin master
git switch -c merge-vX.Y.Z origin/master
git fetch upstream --tags && git merge vX.Y.Z
```

1. Resolve conflicts and regenerate affected files with that release's tooling.
2. Check incoming `.github/workflows` for Google secrets, publishing destinations,
   or other assumptions that would misfire on the fork; disable or adapt them.
   Upstream v1.5.58 has none; Actions does not invoke its internal Kokoro scripts.
3. Open a PR against **stelaxis/cloud-spanner-emulator:master**, never the parent.
   Wait for the required checks and merge using a merge commit.
4. Wait for the resulting master commit's tested `sha-<7-character SHA>` image to
   publish. Tag that commit `vX.Y.Z-stx.N` and push the tag to `origin`; increment
   `N` for subsequent fork releases of the same upstream version.

## Images and CI

`ghcr.io/stelaxis/cloud-spanner-emulator` provides native `linux/amd64` and
`linux/arm64` images in one manifest:

- `edge` and `sha-<7-character SHA>`: tested pushes to `master`.
- `vX.Y.Z-stx.N`: tags matching `v*-stx.*` promote the existing tested SHA manifest
  by digest, without rebuilding. A missing SHA image or revision mismatch fails
  promotion; finish the master build and rerun the release job.

### What runs when

| Event | amd64 build + 8 test shards | arm64 build | Cache | Publishes |
| --- | --- | --- | --- | --- |
| PR to `master` | yes | no; `Build (arm64)` is skipped | read-only (reader) | no |
| Push to `master` | yes | yes | read-write (writer) | `edge`, `sha-…` |
| `workflow_dispatch` on `master` | yes | yes | read-write (writer) | no |
| `workflow_dispatch` on another branch | yes | yes | read-only (reader) | no |
| Push of a `v*-stx.*` tag | no | no | none | promotes `sha-…` |

Pull requests do not build arm64. GitHub counts a job skipped by its `if:` as
passing, so the required `Build (arm64)` check passes on PRs without a runner.
On every other event its condition is true: it runs the real build and, like
before, fails rather than skips if `Dependencies (arm64)` did not succeed.
Publishing needs both `Build` jobs and the test aggregate, so `edge` and every
`sha-…` image, and hence every release tag, has a passing arm64 build. An
arm64-only breakage therefore surfaces on the `master` push after the merge,
not on the PR; run the workflow on the PR branch with `workflow_dispatch` to
check arm64 before merging.

### Bazel on the runner

CI runs Bazel on the runner inside a toolchain container, not in `docker build`.
Each job starts the container from the `toolchain` stage of
`build/docker/Dockerfile.ubuntu` (`test-toolchain` on amd64, which adds the
pinned Google Cloud CLI for `tests/gcloud`), with the checkout at `/src` and a
fixed output base, so actions have the same paths in every job and run. Each
architecture builds that image once per run; later jobs load the same archive.

`Build` jobs build `//binaries:emulator_main` and `//binaries:gateway_main`, then
stage exactly the files the release stage copies (the two binaries, the
toolchain's `libstdc++.so.6` and `licenses.txt.gz`, from the Dockerfile's
license loop) at their `build`-stage paths. The runtime image is built with
`--build-context build=<staged dir>`: a named context replaces the Dockerfile's
`build` stage, so the release stage itself, and with it the gateway, entrypoint,
ports and environment handling, is unchanged. The smoke test runs against that
image as before.

CI flags live in the `ci` config in `.bazelrc`, including `--jobs=2`: generated
C++ files exceed 5 GiB per compiler, and two workers leave room for Bazel,
linking and the OS on 16-GiB runners. Raising it needs CI peak-memory evidence.
Local builds keep `--jobs=auto`.

### Remote cache

Bazel caches action outputs and test results in a GCS bucket over its HTTP cache
protocol (`--remote_cache=https://storage.googleapis.com/<bucket>`). Unchanged
tests report `(cached) PASSED` and do not run. Jobs download only what local
actions need (`--remote_download_minimal`; `Build` jobs also fetch their
binaries).

Configure it with these repository **variables** (Settings → Secrets and
variables → Actions → Variables); none is secret:

| Variable | Value |
| --- | --- |
| `BAZEL_CACHE_BUCKET` | Bucket name, e.g. `stelaxis-bazel-cache-staging` |
| `GCP_WIF_PROVIDER` | `projects/<number>/locations/global/workloadIdentityPools/<pool>/providers/<provider>` |
| `GCP_CACHE_SERVICE_ACCOUNT` | Writer: service account email with object read/write on the bucket |
| `GCP_CACHE_READER_SERVICE_ACCOUNT` | Reader: service account email with object read-only (`objectViewer`) on the bucket |

`google-github-actions/auth` exchanges the job's GitHub OIDC token through
Workload Identity Federation and writes a short-lived credentials file that is
mounted into the container for `--google_credentials`. Only the Bazel jobs have
`id-token: write`.

**PRs can read the cache but never write it; IAM enforces this.** Pushes to
`master` and `workflow_dispatch` runs on `master` impersonate the writer and
upload. Every other run, including every PR and every dispatch on another
branch, impersonates the reader and also passes `--noremote_upload_local_results`,
so Bazel does not attempt uploads the reader would be refused. The workflow's
choice is not the safeguard: a PR can edit the workflow to request the writer, but
the writer is impersonable only by the OIDC subject
`repo:stelaxis/cloud-spanner-emulator:ref:refs/heads/master`, which only runs
on the `master` ref present. A PR run's subject is `…:pull_request`; the reader is
impersonable by any run of this repository. Pushes to `master` require a
reviewed PR.

**No Bazel job may declare an `environment:`.** A job with an environment
presents the subject `repo:…:environment:<name>` instead of the ref, so the
writer binding would refuse even `master`.

| Event and ref | Identity | Uploads |
| --- | --- | --- |
| Push to `master` | writer | yes |
| `workflow_dispatch` on `master` | writer | yes |
| `workflow_dispatch` on another branch | reader | no |
| PR from this repository | reader | no |
| PR from a fork | none, builds cold | no |
| Tag push | none, no Bazel jobs | no |

If `BAZEL_CACHE_BUCKET` or `GCP_WIF_PROVIDER` is unset, or the run's own
account variable is unset (the writer on `master`, the reader elsewhere), the
run has no credentials and builds cold with no remote cache; it still goes
green, just slowly. A `master` run never falls back to the reader.

Bazel does not hash the system compiler or headers into action keys. The
toolchain stage records the architecture, the Bazel version and the installed
versions of GCC, libstdc++, libc6-dev, binutils, protoc and Python in
`/etc/bazel-toolchain.txt`; its hash is the `cache-silo-key` exec property,
which is part of every action key. When one of those packages changes, CI
starts a fresh cache rather than mixing objects from two compilers. Each job
summary shows the cache mode, the key and its inputs.

`linux-libc-dev` (kernel UAPI headers) is deliberately left out. Ubuntu updates
it with every kernel security release, every few weeks, and each change would
discard the whole cache; those headers are a stable ABI. The gcloud CLI is also
not in the key, because only tests run it: its version is part of `GCLOUD_DIR`,
and `--test_env=GCLOUD_DIR` puts that in every test's key. A new CLI re-runs
the tests but reuses compiled outputs.

The bucket deletes objects 30 days after they were written, even if they are
still read. Bazel retries a build (`--experimental_remote_cache_eviction_retries`)
when an entry disappears mid-build, and the next `master` run rewrites what it
had to rebuild.

**Warming the cache.** Every `master` push warms it. To warm it without a
push, for example after creating the bucket or after a toolchain change:

```sh
gh workflow run emulator.yml --repo stelaxis/cloud-spanner-emulator --ref master
```

A cold `master` run takes about as long as CI did before the cache (hours, see
below). Until it finishes, PRs get few cache hits.

### Jobs and the cold path

The job chains are `Build (amd64)` → eight test shards → the aggregate, and, off
PRs, `Dependencies (arm64)` → `Build (arm64)`. A cold build must still fit
GitHub's six-hour job limit whether the cache is empty, evicted or unset:

- `Build (amd64)` compiles gRPC, protobuf, GoogleSQL and the emulator in one job.
  Cold, the previous stages took about 34 + 67 + 137 minutes, around four hours.
- arm64 is slower: about 35 + 96 minutes for dependencies and up to 255 for the
  emulator. `Dependencies (arm64)` builds gRPC, protobuf and the GoogleSQL
  analyzer in its own job, so each arm64 job stays under about 4.5 hours.
- Each Bazel job that others depend on uploads a `bazel-handoff-<arch>` artifact:
  the toolchain image and a Bazel `--disk_cache` holding every result it
  computed locally. The next jobs use it alongside the remote cache. Without a
  writable remote cache (PRs, unset variables, forks), this is what stops each
  test shard from recompiling the dependencies; with a warm cache the disk
  cache is small.
  Artifacts expire after three days; rerun the producer if they have.

The GitHub Actions cache is not used: it cannot hold these outputs within the
10-GB repository quota.

### Tests

Each test shard runs `bazel test --config=ci --` followed by its target
patterns on native amd64. The shards depend only on `Build (amd64)`.

| Shard | Patterns |
| --- | --- |
| backend-query | `//backend/query/...` |
| backend-schema | `//backend/schema/...` |
| backend-other | `//backend/... -//backend/query/... -//backend/schema/...` |
| frontend | `//frontend/...` |
| conformance | `//tests/conformance/...` |
| postgres-functions | `//third_party/spanner_pg/function_evaluators/...` |
| postgres-other | `//third_party/spanner_pg/... -//third_party/spanner_pg/src/... -//third_party/spanner_pg/function_evaluators/...` |
| remaining | `//... -//backend/... -//frontend/... -//tests/conformance/... -//third_party/spanner_pg/...` |

These disjoint package selections union to the original `...` minus
`third_party/spanner_pg/src/...`; the complement also catches new areas. A static
inventory of tracked `BUILD`/`BUILD.bazel` files verified every included package
and all 227 `cc_test` plus six `py_test` declarations belong to exactly one shard.
The one `test_suite` expands only tests in its own package; four other `cc_test`
declarations are under the excluded PostgreSQL `src` tree. These are target
counts, not test-case counts.

The shards stay because of the cold path: test-only compilation took 14–100
minutes per shard in the last cold run, about eight runner-hours in total, which
one six-hour job cannot hold. With a warm cache most shards finish in minutes.

The required `Upstream unit and conformance tests` aggregate fails unless
**every shard succeeds**, including when the matrix is skipped by a failed amd64
build. Its `always()` condition also evaluates cancelled runs and rejects
non-success. Failures upload `bazel-testlogs-<area>` and the full console log
for seven days. Job summaries include `df -h` and the handoff size.

### Publishing

Publication waits for both native smoke tests and the full suite. Only publication
and release promotion receive `packages: write` via `GITHUB_TOKEN`; they do not
check out or execute repository code. Both validate a dry-run manifest before
publishing public tags. No external registry secrets are needed.

The per-run `build-<run>-<attempt>-<arch>` staging tags are disposable and accumulate.
They may be removed during retention maintenance, but preserve the underlying
manifests referenced by retained multi-arch images. Keep Actions enabled, permit
the repository's token to publish GHCR packages, and set package visibility to
public if anonymous pulls are wanted.

## Local build on macOS arm64

Use Docker Desktop or OrbStack with a native arm64 Linux engine. Do not select an
amd64 platform or install QEMU. From the repository root:

```sh
docker build -f build/docker/Dockerfile.ubuntu -t stelaxis-emulator:local .
docker run --rm --name stelaxis-emulator -p 9010:9010 -p 9020:9020 stelaxis-emulator:local
# In another terminal:
curl --fail localhost:9020/v1/projects/test/instances
```

The default Docker platform on an arm64 engine is arm64. The local build compiles
everything in Docker and does not use the remote cache. Upstream selects
`build --jobs=auto` in `.bazelrc`; this fork's `ARG BAZEL_JOBS=auto` preserves that
local default. Use `--build-arg BAZEL_JOBS=2` to limit memory use on smaller Docker
VMs. Allow ample disk space and retain the build cache: the compiled dependencies
are their own `deps` layer.
