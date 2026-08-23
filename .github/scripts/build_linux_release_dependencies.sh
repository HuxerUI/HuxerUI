#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
  printf 'Usage: build_linux_release_dependencies.sh <prefix> <build-directory>\n' >&2
  exit 1
fi

prefix=$1
build_directory=$2
glib_source="$build_directory/glib"
glib_build="$build_directory/glib-build"
libsoup_source="$build_directory/libsoup"
libsoup_build="$build_directory/libsoup-build"

mkdir -p "$prefix" "$build_directory"

git -c advice.detachedHead=false clone --depth 1 --branch 2.70.5 \
  https://gitlab.gnome.org/GNOME/glib.git "$glib_source"
meson setup "$glib_build" "$glib_source" \
  --prefix="$prefix" \
  --libdir=lib \
  --buildtype=release \
  -Dtests=false \
  -Dinstalled_tests=false \
  -Dselinux=disabled \
  -Dlibmount=disabled \
  -Dnls=disabled \
  -Dlibelf=disabled \
  -Dxattr=false
meson compile -C "$glib_build"
meson install -C "$glib_build"

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

git -c advice.detachedHead=false clone --depth 1 --branch 3.0.7 \
  https://gitlab.gnome.org/GNOME/libsoup.git "$libsoup_source"
meson setup "$libsoup_build" "$libsoup_source" \
  --prefix="$prefix" \
  --libdir=lib \
  --buildtype=release \
  -Dtests=false \
  -Dintrospection=disabled \
  -Dvapi=disabled \
  -Dgtk_doc=false \
  -Dsysprof=disabled \
  -Dautobahn=disabled \
  -Dhttp2_tests=disabled \
  -Dpkcs11_tests=disabled \
  -Dtls_check=false
meson compile -C "$libsoup_build"
meson install -C "$libsoup_build"
