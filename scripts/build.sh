#!/usr/bin/env bash
#
# Build the Ki Blast Arena .self using the Dockerized PSL1GHT toolchain (the
# canonical flow). No local toolchain install required.
#
# Usage:
#   ./scripts/build.sh            # build -> src.self (named after the /src mount)
#   ./scripts/build.sh clean      # clean the build
#   ./scripts/build.sh pkg        # build an installable PKG
#
# Env:
#   PS3_TOOLCHAIN_IMAGE   Docker image to use
#                         (default: ghcr.io/02900/ps3-toolchain:latest)
#
# Note: on Apple Silicon add `--platform linux/amd64` to the docker run below.
#
set -euo pipefail

IMAGE="${PS3_TOOLCHAIN_IMAGE:-ghcr.io/02900/ps3-toolchain:latest}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TARGET=""
case "${1:-build}" in
  build)      TARGET="" ;;
  clean)      TARGET="clean" ;;
  pkg)        TARGET="pkg" ;;
  *)          echo "Unknown target '${1}'. Use: build | clean | pkg" >&2; exit 1 ;;
esac

echo ">> Building with $IMAGE (make $TARGET)"
docker run --rm \
  -v "$REPO_ROOT":/src -w /src \
  "$IMAGE" \
  make $TARGET
