#!/bin/sh
set -eu

sdk=$1
case $sdk in
    iphoneos) triple=arm64-apple-ios16.0; subsystem=ios ;;
    appletvos) triple=arm64-apple-tvos16.0; subsystem=tvos ;;
    iphonesimulator) triple=arm64-apple-ios16.0-simulator; subsystem=ios-simulator ;;
    appletvsimulator) triple=arm64-apple-tvos16.0-simulator; subsystem=tvos-simulator ;;
    *) echo "unknown sdk $sdk"; exit 2 ;;
esac

src=$PWD
work=${WORK:-$PWD/ios-build}/$sdk
prefix=$work/prefix
sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)
flags="'-isysroot', '$sysroot', '-target', '$triple'"
mkdir -p "$work" "$prefix"

cat > "$work/cross.txt" <<CROSS
[binaries]
c = 'clang'
cpp = 'clang++'
objc = 'clang'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = [$flags]
c_link_args = [$flags]
cpp_args = [$flags]
cpp_link_args = [$flags]
objc_args = [$flags, '-Wno-error=deprecated', '-Wno-error=deprecated-declarations']
objc_link_args = [$flags]

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'darwin'
subsystem = '$subsystem'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
CROSS

export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=

fetch() {
    [ -d "$work/$1" ] || git clone -q --depth 1 --recursive --branch "$3" "$2" "$work/$1"
}

meson_dep() {
    name=$1; shift
    [ -f "$work/$name/.done" ] && return
    meson setup "$work/$name/build" "$work/$name" --cross-file "$work/cross.txt" \
        --prefix "$prefix" --libdir lib --buildtype release --default-library static "$@"
    meson install -C "$work/$name/build"
    touch "$work/$name/.done"
}

fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git n8.0
if [ ! -f "$work/ffmpeg/.done" ]; then
    mkdir -p "$work/ffmpeg/build"
    cd "$work/ffmpeg/build"
    ../configure --prefix="$prefix" --enable-cross-compile --target-os=darwin --arch=aarch64 \
        --cc=clang --sysroot="$sysroot" --extra-cflags="-target $triple" \
        --extra-ldflags="-target $triple" --enable-static --disable-shared \
        --disable-programs --disable-doc --disable-debug --disable-avdevice \
        --enable-videotoolbox --enable-audiotoolbox
    make -j"$(sysctl -n hw.ncpu)" install
    touch "$work/ffmpeg/.done"
    cd "$src"
fi

fetch freetype https://gitlab.freedesktop.org/freetype/freetype.git VER-2-13-3
meson_dep freetype -Dharfbuzz=disabled -Dpng=disabled -Dbrotli=disabled \
    -Dbzip2=disabled -Dzlib=disabled

fetch fribidi https://github.com/fribidi/fribidi.git v1.0.16
meson_dep fribidi -Ddocs=false -Dtests=false -Dbin=false

fetch harfbuzz https://github.com/harfbuzz/harfbuzz.git 11.2.1
meson_dep harfbuzz -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled \
    -Dcairo=disabled -Dicu=disabled -Dtests=disabled -Ddocs=disabled \
    -Dutilities=disabled -Dintrospection=disabled

fetch libass https://github.com/libass/libass.git 0.17.4
meson_dep libass -Dfontconfig=disabled -Dcoretext=enabled -Dlibunibreak=disabled \
    -Drequire-system-font-provider=false -Dasm=disabled -Dtest=disabled

fetch libplacebo https://code.videolan.org/videolan/libplacebo.git v7.360.1
meson_dep libplacebo -Dvulkan=disabled -Dopengl=disabled -Dd3d11=disabled \
    -Dglslang=disabled -Dshaderc=disabled -Dlcms=disabled -Ddovi=enabled \
    -Dlibdovi=disabled -Dxxhash=disabled -Dunwind=disabled -Ddemos=false -Dtests=false

meson setup "$work/mpv" "$src" --cross-file "$work/cross.txt" --prefix "$prefix" \
    --libdir lib --buildtype release --default-library static --werror \
    -Dlibmpv=true -Dcplayer=false -Dlua=disabled -Djavascript=disabled \
    -Dcocoa=disabled -Dswift-build=disabled -Dmacos-cocoa-cb=disabled \
    -Dcoreaudio=disabled -Dvulkan=disabled -Dmanpage-build=disabled \
    -Davfoundation=enabled -Daudiounit=enabled
meson compile -C "$work/mpv"
meson install -C "$work/mpv"

printf '#include <mpv/client.h>\nint main(void) { mpv_handle *h = mpv_create(); mpv_initialize(h); mpv_terminate_destroy(h); return 0; }\n' > "$work/link.c"
clang -isysroot "$sysroot" -target "$triple" -o "$work/link" "$work/link.c" \
    $(pkg-config --cflags --libs --static mpv) -lc++
echo "linked $sdk"

out=$work/out
rm -rf "$out"
mkdir -p "$out/include/mpv"
libtool -static -o "$out/libmpv.a" "$prefix"/lib/*.a
cp "$prefix"/include/mpv/*.h "$out/include/mpv/"
cat > "$out/include/module.modulemap" <<MAP
module Libmpv {
    header "mpv/client.h"
    header "mpv/render.h"
    link "c++"
    link "z"
    link "bz2"
    link "iconv"
    link framework "AVFoundation"
    link framework "AudioToolbox"
    link framework "CoreMedia"
    link framework "CoreText"
    link framework "CoreVideo"
    link framework "VideoToolbox"
    export *
}
MAP
