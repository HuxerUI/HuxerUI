#!/bin/sh

set -eu

if [ "$#" -ne 4 ]; then
  printf 'usage: check_macos_platform_bridge.sh <source-dir> <headers-dir> <library> <build-dir>\n' >&2
  exit 1
fi

source_directory=$1
headers_directory=$2
library_path=$3
build_directory=$4
module_headers="$build_directory/headers"

mkdir -p "$module_headers"
cp -R "$headers_directory/." "$module_headers/"
cp "$source_directory/platform/macos/HuxerUIPlatform.modulemap" "$module_headers/module.modulemap"

xcrun --sdk macosx clang \
  -fsyntax-only \
  -fobjc-arc \
  -fmodules \
  -I "$module_headers" \
  "$source_directory/tests/platform/macos_platform_registry.m"
xcrun --sdk macosx swiftc \
  -I "$module_headers" \
  "$source_directory/tests/platform/macos_platform_registry.swift" \
  "$library_path" \
  -lc++ \
  -framework AppKit \
  -framework Carbon \
  -framework CoreGraphics \
  -framework CoreImage \
  -framework CoreText \
  -framework CoreVideo \
  -framework Foundation \
  -framework ImageIO \
  -framework QuartzCore \
  -Xlinker -weak_framework \
  -Xlinker UniformTypeIdentifiers \
  -o "$build_directory/huxerui_macos_swift_link_test"
