#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="xiaoai-plus-toolchain:dev"
SHERPA_VERSION="v1.12.25"
BUILD_JOBS="${BUILD_JOBS:-2}"
SHERPA_SRC_DIR="${PROJECT_ROOT}/assets/sherpa-onnx-src"
SHERPA_STATIC_DIR="${PROJECT_ROOT}/assets/sherpa_onnx_static"

# Ensure Docker image exists.
HOST_PROJECT_ROOT="${PROJECT_ROOT}"
if command -v cygpath >/dev/null 2>&1; then
  HOST_PROJECT_ROOT="$(cygpath -am "${PROJECT_ROOT}")"
fi
docker build --platform linux/amd64 -t "${IMAGE_NAME}" "${HOST_PROJECT_ROOT}/toolchain"

# Clone sherpa-onnx source if not present.
if [ ! -d "${SHERPA_SRC_DIR}" ]; then
  echo "==> Cloning sherpa-onnx ${SHERPA_VERSION} ..."
  git -c core.autocrlf=false clone --depth 1 --branch "${SHERPA_VERSION}" \
    https://github.com/k2-fsa/sherpa-onnx.git "${SHERPA_SRC_DIR}"
fi
if [[ "$(git -C "${SHERPA_SRC_DIR}" describe --tags --exact-match)" != "${SHERPA_VERSION}" ]]; then
  echo "sherpa-onnx source commit differs from reviewed ${SHERPA_VERSION}" >&2
  exit 1
fi

# Expose KWS match scores and disable ONNX worker spinning.
patch_path="${PROJECT_ROOT}/toolchain/sherpa-kws-observability.patch"
if git -C "${SHERPA_SRC_DIR}" apply --reverse --check "${patch_path}" 2>/dev/null; then
  :
else
  git -C "${SHERPA_SRC_DIR}" apply "${patch_path}"
fi

echo "==> Building sherpa-onnx static libraries for ARM ..."
MSYS_NO_PATHCONV=1 docker run --rm --platform linux/amd64 \
  -e BUILD_JOBS="${BUILD_JOBS}" \
  -v "${HOST_PROJECT_ROOT}":/workspace \
  -w /workspace/assets/sherpa-onnx-src \
  "${IMAGE_NAME}" \
  bash -lc '
    mkdir -p build-arm-static && cd build-arm-static
    SYSROOT=/opt/sysroot cmake .. \
      -DCMAKE_TOOLCHAIN_FILE=/workspace/armv7-linux-gnueabihf.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/workspace/assets/sherpa_onnx_static \
      -DBUILD_SHARED_LIBS=OFF \
      -DSHERPA_ONNX_ENABLE_C_API=ON \
      -DSHERPA_ONNX_ENABLE_BINARY=OFF \
      -DSHERPA_ONNX_ENABLE_TESTS=OFF \
      -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
      -DSHERPA_ONNX_ENABLE_TTS=OFF \
      -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
      -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
      -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF
    cmake --build . --parallel "${BUILD_JOBS}"
    cmake --install .
  '

echo "==> Static libraries installed to ${SHERPA_STATIC_DIR}"
ls -lh "${SHERPA_STATIC_DIR}/lib/"*.a 2>/dev/null || ls -lhR "${SHERPA_STATIC_DIR}/lib/"
