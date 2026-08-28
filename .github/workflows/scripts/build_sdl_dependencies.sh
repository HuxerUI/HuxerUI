#!/usr/bin/env bash

set -euo pipefail

sdl_install_prefix=${1:-/usr/local}
sdl_parallel_jobs=${2:-4}
sdl_source_root=$(mktemp -d)
trap 'rm -rf "${sdl_source_root}"' EXIT

clone_sdl_repository() {
  local repository=$1
  local tag=$2
  local destination=$3
  local attempt
  for attempt in 1 2 3; do
    if git clone --quiet --depth 1 --branch "${tag}" "${repository}" "${destination}"; then
      return
    fi
    rm -rf "${destination}"
    sleep "${attempt}"
  done
  return 1
}

clone_sdl_repository https://github.com/libsdl-org/SDL.git release-3.4.0 "${sdl_source_root}/SDL"
clone_sdl_repository https://github.com/libsdl-org/SDL_image.git release-3.4.0 "${sdl_source_root}/SDL_image"
clone_sdl_repository https://github.com/libsdl-org/SDL_ttf.git release-3.2.2 "${sdl_source_root}/SDL_ttf"

cmake -S "${sdl_source_root}/SDL" -B "${sdl_source_root}/build-sdl" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${sdl_install_prefix}" \
  -DSDL_SHARED=ON \
  -DSDL_STATIC=OFF \
  -DSDL_TEST_LIBRARY=OFF \
  -DSDL_TESTS=OFF
cmake --build "${sdl_source_root}/build-sdl" --parallel "${sdl_parallel_jobs}"
cmake --install "${sdl_source_root}/build-sdl"

cmake -S "${sdl_source_root}/SDL_image" -B "${sdl_source_root}/build-sdl-image" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${sdl_install_prefix}" \
  -DCMAKE_PREFIX_PATH="${sdl_install_prefix}" \
  -DBUILD_SHARED_LIBS=ON \
  -DSDLIMAGE_INSTALL=ON \
  -DSDLIMAGE_SAMPLES=OFF \
  -DSDLIMAGE_STRICT=ON \
  -DSDLIMAGE_TESTS=OFF \
  -DSDLIMAGE_VENDORED=OFF \
  -DSDLIMAGE_AVIF=OFF \
  -DSDLIMAGE_JXL=OFF \
  -DSDLIMAGE_TIF=OFF \
  -DSDLIMAGE_WEBP=OFF
cmake --build "${sdl_source_root}/build-sdl-image" --parallel "${sdl_parallel_jobs}"
cmake --install "${sdl_source_root}/build-sdl-image"

cmake -S "${sdl_source_root}/SDL_ttf" -B "${sdl_source_root}/build-sdl-ttf" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${sdl_install_prefix}" \
  -DCMAKE_PREFIX_PATH="${sdl_install_prefix}" \
  -DBUILD_SHARED_LIBS=ON \
  -DSDLTTF_INSTALL=ON \
  -DSDLTTF_PLUTOSVG=OFF \
  -DSDLTTF_SAMPLES=OFF \
  -DSDLTTF_STRICT=ON \
  -DSDLTTF_VENDORED=OFF
cmake --build "${sdl_source_root}/build-sdl-ttf" --parallel "${sdl_parallel_jobs}"
cmake --install "${sdl_source_root}/build-sdl-ttf"
