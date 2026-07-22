#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 6 ]; then
	echo "usage: $0 <superbuild-source-dir> <source-dir> <build-dir> <target> <app-id> <output-apk>" >&2
	exit 64
fi

SUPERBUILD_SOURCE_DIR=$(cd "$1" && pwd)
SOURCE_DIR=$(cd "$2" && pwd)
BUILD_DIR=$(mkdir -p "$3" && cd "$3" && pwd)
TARGET=$4
APP_ID=$5
OUTPUT_APK=$6

ANDROID_INSTALL_ROOT=${ANDROID_INSTALL_ROOT:-"$BUILD_DIR/android-toolchain"}
BUILD_PARALLELISM=${BUILD_PARALLELISM:-$(nproc)}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Unix Makefiles}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$(dirname "$OUTPUT_APK")"

target_install_dir="$BUILD_DIR/$TARGET/install"

# Force superbuild to provision a consistent SDK / NDK instead of inheriting
# whatever the hosted runner happens to expose.
unset ANDROID_HOME
unset ANDROID_NDK_ROOT
unset ANDROID_SDK_ROOT

superbuild_patch="$SCRIPT_DIR/superbuild-qt-5.12.10-gcc11.patch"
if ! grep -q 'qtbase-gcc-11-moc.patch' "$SUPERBUILD_SOURCE_DIR/qt-5.12.10.cmake"; then
	patch -d "$SUPERBUILD_SOURCE_DIR" -p1 < "$superbuild_patch"
fi

download_github_raw_file()
{
	local output=$1
	local api_url=$2
	local curl_args=(-fsSL -H "Accept: application/vnd.github.raw")

	if [ -n "${GITHUB_TOKEN:-}" ]; then
		curl_args+=(-H "Authorization: Bearer $GITHUB_TOKEN")
		curl_args+=(-H "X-GitHub-Api-Version: 2022-11-28")
	fi

	curl "${curl_args[@]}" "$api_url" -o "$output"
}

libkml_cmake="$SUPERBUILD_SOURCE_DIR/libkml-1.3.0.cmake"
if grep -q 'raw.githubusercontent.com/msys2/MINGW-packages' "$libkml_cmake"; then
	msys2_ref=d5535441b7435a0e7a7d7dee83b175eae7b48475
	msys2_api_base=https://api.github.com/repos/msys2/MINGW-packages/contents/mingw-w64-libkml

	# The pinned superbuild later reads these helper downloads via
	# <DOWNLOAD_DIR>, so pre-populate the exact DOWNLOAD_NAME files while
	# preserving the superbuild's SHA256 verification.
	download_github_raw_file \
		"$SUPERBUILD_SOURCE_DIR/libkml-1.3.0-mingw.patch" \
		"$msys2_api_base/001-libkml-1.3.0.patch?ref=$msys2_ref"
	download_github_raw_file \
		"$SUPERBUILD_SOURCE_DIR/libkml-1.3.0-strptime.c" \
		"$msys2_api_base/strptime.c?ref=$msys2_ref"
fi

# RelWithDebInfo still produces a release-style APK, but avoids the superbuild
# toolchain's hard failure on unsigned pure Release packaging.
cmake -S "$SUPERBUILD_SOURCE_DIR" -B "$BUILD_DIR" -G "$CMAKE_GENERATOR" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	"-DMapper_CI_SOURCE_DIR=$SOURCE_DIR" \
	"-DMapper_CI_VERSION_DISPLAY=0.9.7-COC.5" \
	"-DMapper_CI_APP_ID=$APP_ID" \
	"-DMapper_CI_GDAL_DATA_DIR=$target_install_dir/usr/share/gdal" \
	"-DENABLE_$TARGET=1" \
	"-D${TARGET}_INSTALL_PREFIX=/usr" \
	"-DANDROID_SDK_INSTALL_ROOT=$ANDROID_INSTALL_ROOT" \
	"-DANDROID_NDK_INSTALL_ROOT=$ANDROID_INSTALL_ROOT" \
	-DANDROID_BUILD_LIBCXX=1

cmake --build "$BUILD_DIR" \
	--target "openorienteering-mapper-ci-$TARGET" \
	--parallel "$BUILD_PARALLELISM"

apk=$(find "$BUILD_DIR/$TARGET/openorienteering-mapper-ci" -type f -name 'OpenOrienteering-Mapper-*.apk' | sort | head -n 1)
if [ -z "$apk" ]; then
	echo "no APK produced under $BUILD_DIR/$TARGET/openorienteering-mapper-ci" >&2
	exit 1
fi

cp "$apk" "$OUTPUT_APK"
