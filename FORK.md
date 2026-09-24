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

PRs to master build both architectures without publishing. PRs and master pushes
run the full upstream unit/conformance selection on native amd64 inside the
loaded builder image, with explicit job/output flags and test-result reuse off:

```sh
bazel test -c opt --jobs="$BAZEL_JOBS" --test_output=errors \
  --nocache_test_results --disk_cache=/bazel-cache -- ... -third_party/spanner_pg/src/...
```

The test image adds a pinned, checksum-verified Google Cloud CLI and `GCLOUD_DIR`
for `tests/gcloud`. Tests run via `docker run`, so BuildKit cannot clip their logs.
Failures upload `bazel-testlogs` and the full console log for seven days.

Separate native jobs checkpoint gRPC/protobuf (`rpc-deps`), GoogleSQL (`deps`),
and the runtime build, each with a six-hour budget. Each architecture has its own
GitHub Actions `mode=max` caches, exported after each checkpoint. Later timeouts
retain earlier checkpoints. Cache-export errors are nonfatal. Tests have a separate
builder-image cache and a persistent Bazel disk cache saved even on failure.
Their four-hour step timeout leaves time in the six-hour job for logs and cache
upload, so a test compilation timeout can still warm the next run.

CI sets `BAZEL_JOBS=2` once for all builds and tests. Observed generated C++ files
exceed 5 GiB per compiler; two workers leave room for Bazel, linking, and the OS
on the 4-vCPU/16-GiB runners. Raising this to three or four needs CI peak-memory
evidence. **Changing `BAZEL_JOBS` invalidates the dependency layers and rebuilds
GoogleSQL from scratch.** Monitor cache eviction/disk use and raise the repository
cache quota if necessary: the checkpoint and test caches can exceed the default.

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

The default Docker platform on an arm64 engine is arm64. The Dockerfile retains
upstream's `BAZEL_JOBS=auto` default; use `--build-arg BAZEL_JOBS=2` to limit memory
use on smaller Docker VMs. Allow ample disk space and retain the build cache.
