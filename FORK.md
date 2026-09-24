# Stelaxis Cloud Spanner Emulator fork

This fork is for Stelaxis development and tests only, not production. It will add
parallel read-write transactions and persistence; Phase 0 only establishes image
builds and an upstream test baseline. Upstream does not accept contributions.

## Updating from upstream

Keep fork changes on `master`. Merge each upstream release; do not rebase:

```sh
git remote add upstream https://github.com/GoogleCloudPlatform/cloud-spanner-emulator.git # once
git fetch origin master
git switch -c merge-vX.Y.Z origin/master
git fetch upstream --tags && git merge vX.Y.Z
```

Resolve conflicts, regenerate any affected generated files with that release's
build tooling, and run the image and test gates. Open a PR against
**stelaxis/cloud-spanner-emulator:master**, never the GoogleCloudPlatform parent.
Merge that PR with a merge commit to preserve upstream ancestry (no squash or
rebase). Tag the tested master commit `vX.Y.Z-stx.N` and push that tag to
`origin` (increment `N` for subsequent fork releases of the same upstream version).

## Images and CI

`ghcr.io/stelaxis/cloud-spanner-emulator` provides native `linux/amd64` and
`linux/arm64` images in one manifest:

- `edge` and `sha-<7-character SHA>`: pushes to `master`.
- `vX.Y.Z-stx.N`: git tags matching `v*-stx.*`.

PRs to master build both architectures without publishing. Every PR, master push,
and release tag runs the full upstream unit/conformance command on native amd64
inside the Dockerfile's `build` stage:

```sh
bazel test -c opt -- ... -third_party/spanner_pg/src/...
```

The test stage adds a versioned, checksum-verified Google Cloud CLI and sets
`GCLOUD_DIR` for the upstream `tests/gcloud` cases; the runtime image is unchanged.

Publication waits for both builds and the tests. Only the publish job receives
`packages: write` via `GITHUB_TOKEN`; no external registry secrets are needed.
Temporary `build-<run>-<attempt>-<arch>` tags hold the per-architecture images used
to assemble the manifest. The upstream v1.5.58 tree has no GitHub workflows to
disable; its Google-internal Kokoro scripts are not invoked by Actions.

Build/test jobs have six-hour timeouts and cap Bazel at two jobs for the 16 GiB
native runners: individual generated C++ files can need over 5 GiB to compile.
Architecture-specific GitHub Actions
`mode=max` layer caches retain the builder's Bazel outputs, including the separate
GoogleSQL dependency layer; tests reuse the amd64 builder and always rerun the
test stage. Cold compilation can take hours. Monitor cache eviction and runner
disk use; the two builder caches can exceed the repository's default cache quota.
Ensure Actions is enabled on the fork, allow its `GITHUB_TOKEN` to publish GHCR
packages, and set the package visibility to public if anonymous pulls are wanted.

## Local build on macOS arm64

Use Docker Desktop or OrbStack with a native arm64 Linux engine. Do not select an
amd64 platform or install QEMU. From the repository root:

```sh
docker build -f build/docker/Dockerfile.ubuntu -t stelaxis-emulator:local .
docker run --rm --name stelaxis-emulator -p 9010:9010 -p 9020:9020 stelaxis-emulator:local
# In another terminal:
curl --fail localhost:9020/v1/projects/test/instances
```

The default Docker platform on an arm64 engine is arm64. Builds default to four
Bazel jobs to limit C++ compiler memory use; use `--build-arg BAZEL_JOBS=2` on a
memory-constrained Docker VM. Allow ample disk space and retain Docker's build
cache between builds.
