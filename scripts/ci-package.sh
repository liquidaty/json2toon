#!/bin/sh
# Build json2toon and stage a distributable binary archive for one platform.
#
# Shared by every build job in .github/workflows/release.yml so the
# build+install+archive sequence lives in exactly one place. Reads:
#
#   PLATFORM      platform label for the archive name (e.g. amd64-linux-gcc)
#   VERSION       release version (e.g. 1.0.0), used in the archive name
#   ARTIFACT_DIR  directory the .tar.gz / .zip archives are written to
#   MAKE          make command (default: make; FreeBSD passes gmake)
#
# The build config (CC/AR/RANLIB/CONFIGURE_HOST/STATIC_BUILD/...) is set by the
# caller before this script runs ./configure implicitly via `make`; this script
# only assumes ./configure has already produced config.mk.
#
# PLATFORM (not PREFIX) names the archive: the Makefile's PREFIX is the install
# root, which we override per-build to a staging dir below.

set -e

: "${PLATFORM:?PLATFORM must be set}"
: "${VERSION:?VERSION must be set}"
: "${ARTIFACT_DIR:?ARTIFACT_DIR must be set}"
MAKE="${MAKE:-make}"

STAGE="json2toon-$VERSION-$PLATFORM"
STAGEDIR="$PWD/$STAGE"

echo "[INF] Packaging $STAGE"
echo "[INF]   PWD=$PWD MAKE=$MAKE"

rm -rf "$STAGEDIR"

# Build the library + CLI, then install (bin/ lib/ include/ pkgconfig/) into the
# staging dir. install depends on the build targets, so this also builds.
"$MAKE" build
"$MAKE" install PREFIX="$STAGEDIR"

# Ship the top-level docs alongside the binaries.
for doc in README.md LICENSE.md; do
  [ -f "$doc" ] && cp -f "$doc" "$STAGEDIR/"
done

mkdir -p "$ARTIFACT_DIR"

# Archive the staging dir so extraction yields a single clean top-level folder.
# Both formats are produced (tar.gz for unix, zip for convenience/Windows).
tar czf "$ARTIFACT_DIR/$STAGE.tar.gz" "$STAGE"
if command -v zip >/dev/null 2>&1; then
  zip -qr "$ARTIFACT_DIR/$STAGE.zip" "$STAGE"
else
  echo "[WRN] zip not found; skipping .zip for $STAGE"
fi

echo "[INF] Archives:"
ls -hl "$ARTIFACT_DIR/$STAGE".* 2>/dev/null || true
