#!/bin/bash
#
# Build and install Sunshine on Haiku.
#
#   packaging/haiku/build.sh [--install-deps] [--prefix DIR] [--jobs N] [--no-vulkan] [--clean]
#
# Steps:
#   1. (optional) install build dependencies with pkgman
#   2. fetch the git submodules Sunshine needs
#   3. build FFmpeg (with Sunshine's CBS helpers) through third-party/build-deps, linking the
#      x264 / x265 / SVT-AV1 encoders from HaikuPorts and Vulkan Video encoders loaded at runtime
#   4. configure, build and install Sunshine and the libvirtualhid input_server add-on
#
# Everything is installed into Haiku's non-packaged hierarchy by default, so nothing touches the
# system packages and removal is a matter of deleting the installed files.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PREFIX="/boot/home/config/non-packaged"
JOBS="$(nproc 2>/dev/null || echo 2)"
INSTALL_DEPS=0
ENABLE_VULKAN=ON
CLEAN=0
BUILD_DIR="${ROOT}/build-haiku"

usage() {
  sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --install-deps) INSTALL_DEPS=1 ;;
    --prefix) PREFIX="$2"; shift ;;
    --jobs) JOBS="$2"; shift ;;
    --no-vulkan) ENABLE_VULKAN=OFF ;;
    --clean) CLEAN=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

if [ "$(uname -s)" != "Haiku" ]; then
  echo "This script builds Sunshine on Haiku; run it on a Haiku system." >&2
  exit 1
fi

if [ "$(uname -m)" != "x86_64" ]; then
  echo "Only x86_64 Haiku is supported at the moment (found $(uname -m))." >&2
  exit 1
fi

log() {
  printf '\n\033[1m==> %s\033[0m\n' "$*"
}

# ---------------------------------------------------------------------------------------------
# 1. Dependencies
#
# Packages are requested by resolvable (cmd:..., devel:...) instead of package name, so this keeps
# working when HaikuPorts bumps package versions (boost1.90 -> boost1.91, nodejs22 -> nodejs24...).
# Boost must come from HaikuPorts: its packages carry Haiku fixes that upstream Boost lacks.
DEPS=(
  cmd:cmake cmd:make cmd:gcc cmd:nasm cmd:pkg_config cmd:git cmd:patch cmd:node cmd:npm
  devel:libssl devel:libcrypto devel:libcurl devel:libminiupnpc
  devel:libboost_filesystem devel:libboost_log devel:libboost_program_options
  devel:libx264 devel:libx265 devel:libSvtAv1Enc
)
if [ "${ENABLE_VULKAN}" = "ON" ]; then
  # Headers only: FFmpeg loads libvulkan at runtime, so machines without a Vulkan driver still work.
  DEPS+=(vulkan_headers)
fi

if [ "${INSTALL_DEPS}" = "1" ]; then
  log "Installing build dependencies"

  # Check that pkgman can reach its repositories before asking it to resolve anything: when it
  # can't, it silently falls back to stale cached data and reports packages as "not found".
  PKGMAN=(pkgman)
  if ! pkgman refresh; then
    # The usual cause is a Haiku build whose Network Kit lacks HTTPS support (the "openssl" build
    # feature), which pkgman reports as "Operation not supported". curl has its own OpenSSL, so
    # if curl can reach the repository server, run pkgman through a local curl relay instead.
    if command -v curl >/dev/null && command -v python3 >/dev/null \
      && curl -sSfL -o /dev/null https://eu.hpkg.haiku-os.org/haikuports/master/x86_64/current/repo.sha256; then
      echo "pkgman can't reach its repositories, but curl can; using the curl relay" \
        "(packaging/haiku/tools/pkgman-curl-relay.py)."
      PKGMAN=(python3 "${ROOT}/packaging/haiku/tools/pkgman-curl-relay.py")
      "${PKGMAN[@]}" refresh
    else
      cat >&2 <<'EOF'
pkgman can't reach its package repositories.
- "Operation not supported" means this Haiku build's Network Kit has no HTTPS support. Haiku
  builds made without an @nightly/@release profile need `HAIKU_BUILD_FEATURE_SSL = 1 ;` in
  build/jam/UserBuildConfig. Until then, packaging/haiku/tools/pkgman-curl-relay.py runs pkgman
  through curl (it needs curl and python3).
- Otherwise, check the network connection and the system clock.
EOF
      exit 1
    fi
  fi

  "${PKGMAN[@]}" install -y "${DEPS[@]}"
fi

# ---------------------------------------------------------------------------------------------
# 2. Sources
log "Fetching submodules"
cd "${ROOT}"
git submodule update --init --depth 1
# build-deps only needs FFmpeg itself; the encoders come from HaikuPorts.
git -C third-party/build-deps submodule update --init --depth 1 third-party/FFmpeg/FFmpeg
git -C third-party/moonlight-common-c submodule update --init --depth 1

# build-deps gains a hook for extra FFmpeg configure flags (patch kept here until it is upstream).
BUILD_DEPS_PATCH="${ROOT}/packaging/haiku/patches/build-deps-extra-configure-flags.patch"
if git -C third-party/build-deps apply --reverse --check "${BUILD_DEPS_PATCH}" 2>/dev/null; then
  echo "build-deps patch already applied"
else
  git -C third-party/build-deps apply "${BUILD_DEPS_PATCH}"
fi

# libvirtualhid gains the Haiku keyboard/mouse backend and its input_server add-on (kept here as
# a patch until the submodule points at a libvirtualhid release that includes it).
LVH_PATCH="${ROOT}/packaging/haiku/patches/libvirtualhid-haiku-backend.patch"
if [ -f third-party/libvirtualhid/src/platform/haiku/haiku_backend.cpp ]; then
  echo "libvirtualhid already has the Haiku backend"
else
  git -C third-party/libvirtualhid apply "${LVH_PATCH}"
fi

if [ "${CLEAN}" = "1" ]; then
  rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}"

# ---------------------------------------------------------------------------------------------
# 3. FFmpeg
FFMPEG_DIR="${BUILD_DIR}/deps/ffmpeg"
FFMPEG_FLAGS="--enable-pic"
FFMPEG_FLAGS+=" --enable-libx264 --enable-encoder=libx264"
FFMPEG_FLAGS+=" --enable-libx265 --enable-encoder=libx265"
FFMPEG_FLAGS+=" --enable-libsvtav1 --enable-encoder=libsvtav1"
if [ "${ENABLE_VULKAN}" = "ON" ]; then
  FFMPEG_FLAGS+=" --enable-vulkan --enable-encoder=h264_vulkan,hevc_vulkan,av1_vulkan"
fi

if [ ! -f "${FFMPEG_DIR}/lib/libavcodec.a" ] || [ ! -f "${FFMPEG_DIR}/lib/libcbs.a" ]; then
  log "Building FFmpeg"
  cmake -S third-party/build-deps -B "${BUILD_DIR}/build-deps" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${BUILD_DIR}/deps" \
    -DPARALLEL_BUILDS="${JOBS}" \
    -DBUILD_ALL=OFF \
    -DBUILD_FFMPEG=ON \
    -DBUILD_FFMPEG_ENCODERS=ON \
    -DBUILD_FFMPEG_DECODERS=OFF \
    -DBUILD_FFMPEG_TOOLS=OFF \
    -DBUILD_FFMPEG_DAV1D=OFF \
    -DBUILD_FFMPEG_OPUS=ON \
    -DBUILD_FFMPEG_CBS=ON \
    -DBUILD_FFMPEG_AMF=OFF \
    -DBUILD_FFMPEG_MF=OFF \
    -DBUILD_FFMPEG_NV_CODEC_HEADERS=OFF \
    -DBUILD_FFMPEG_CUDA_LLVM=OFF \
    -DBUILD_FFMPEG_LIBVA=OFF \
    -DBUILD_FFMPEG_V4L2=OFF \
    -DBUILD_FFMPEG_VULKAN=OFF \
    -DBUILD_FFMPEG_X264=OFF \
    -DBUILD_FFMPEG_X265=OFF \
    -DBUILD_FFMPEG_SVT_AV1=OFF \
    -DFFMPEG_EXTRA_CONFIGURE_FLAGS="${FFMPEG_FLAGS}"
  cmake --build "${BUILD_DIR}/build-deps" --parallel "${JOBS}"
  cmake --install "${BUILD_DIR}/build-deps"
else
  echo "Using existing FFmpeg in ${FFMPEG_DIR} (pass --clean to rebuild)"
fi

# ---------------------------------------------------------------------------------------------
# 4. Sunshine
log "Configuring Sunshine"
cmake -S "${ROOT}" -B "${BUILD_DIR}/sunshine" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DSUNSHINE_ASSETS_DIR=share/sunshine \
  -DSUNSHINE_ENABLE_TRAY=OFF \
  -DSUNSHINE_ENABLE_VULKAN="${ENABLE_VULKAN}" \
  -DBOOST_USE_STATIC=OFF \
  -DFFMPEG_PREPARED_BINARIES="${FFMPEG_DIR}" \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF

log "Building Sunshine"
cmake --build "${BUILD_DIR}/sunshine" --parallel "${JOBS}"

log "Installing to ${PREFIX}"
cmake --install "${BUILD_DIR}/sunshine"

ADDON="${PREFIX}/add-ons/input_server/devices/virtualhid_input"
cat <<EOF

Sunshine is installed:
  program:        ${PREFIX}/bin/sunshine
  input add-on:   ${ADDON}
  settings, logs: ~/config/settings/Sunshine

Next steps:
  1. Restart input_server so it loads the input add-on (rebooting is the simplest way).
     Without it, clients can watch but not control this computer.
  2. Run 'sunshine' and open https://localhost:47990 to set a username and password.
  3. Pair Moonlight. If it does not find this computer, add it by IP address.
EOF
