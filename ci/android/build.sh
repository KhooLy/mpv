#!/bin/bash
set -eu

ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK:-$HOME/Android/Sdk/ndk/27.1.12297006}}
api=28
abi=${ABI:-arm64-v8a}
case $abi in
    arm64-v8a) target=aarch64-linux-android; triple=$target; rtarget=$target; family=aarch64; cpu=arm64; ffarch=aarch64 ;;
    armeabi-v7a) target=armv7a-linux-androideabi; triple=arm-linux-androideabi; rtarget=armv7-linux-androideabi; family=arm; cpu=armv7; ffarch=arm ;;
    x86_64) target=x86_64-linux-android; triple=$target; rtarget=$target; family=x86_64; cpu=x86_64; ffarch=x86_64 ;;
    *) echo "unknown abi $abi"; exit 2 ;;
esac
host=$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64
tc=$ndk/toolchains/llvm/prebuilt/$host
cc=$tc/bin/$target$api-clang
luajit_host=gcc
[ "$family" = arm ] && luajit_host="gcc -m32"
jobs=$(nproc)

src=$(cd "$(dirname "$0")/../.." && pwd)
work=${WORK:-$src/android-build}
prefix=$work/prefix
mkdir -p "$work" "$prefix"

cat > "$work/cross.ini" <<CROSS
[binaries]
c = '$cc'
cpp = '$cc++'
ar = '$tc/bin/llvm-ar'
strip = '$tc/bin/llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']

[host_machine]
system = 'android'
cpu_family = '$family'
cpu = '$cpu'
endian = 'little'
CROSS

export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH=

fetch() {
    [ -d "$work/$1" ] && return
    git init -q "$work/$1"
    git -C "$work/$1" fetch -q --depth 1 "$2" "$3"
    git -C "$work/$1" checkout -q FETCH_HEAD
    git -C "$work/$1" submodule -q update --init --depth 1
    if [ -n "${4:-}" ]; then
        git -C "$work/$1" apply "$src/third_party/$4"
    fi
}

meson_dep() {
    name=$1; shift
    [ -f "$work/$name/.done" ] && return
    meson setup "$work/$name/build" "$work/$name" --cross-file "$work/cross.ini" \
        --prefix "$prefix" --libdir lib --buildtype release --default-library static \
        -Db_staticpic=true "$@"
    meson install -C "$work/$name/build"
    touch "$work/$name/.done"
}

cmake_dep() {
    name=$1; shift
    [ -f "$work/$name/.done" ] && return
    cmake -S "$work/$name" -B "$work/$name/build" \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$abi -DANDROID_PLATFORM=$api -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$work/$name/build" -j"$jobs"
    cmake --install "$work/$name/build"
    touch "$work/$name/.done"
}

fetch mbedtls https://github.com/Mbed-TLS/mbedtls.git v3.6.4
cmake_dep mbedtls -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF

fetch libxml2 https://gitlab.gnome.org/GNOME/libxml2.git v2.14.5
cmake_dep libxml2 -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF \
    -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_TESTS=OFF \
    -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_ICU=OFF

fetch dav1d https://code.videolan.org/videolan/dav1d.git 1.5.1
meson_dep dav1d -Denable_tools=false -Denable_tests=false

fetch luajit https://github.com/LuaJIT/LuaJIT.git c6ffc141a8762b41703f9287d63d93622a13dd8f
if [ ! -f "$work/luajit/.done" ]; then
    make -C "$work/luajit" -j"$jobs" HOST_CC="$luajit_host" CROSS="$tc/bin/llvm-" \
        STATIC_CC="$cc" DYNAMIC_CC="$cc -fPIC" TARGET_LD="$cc" \
        TARGET_AR="$tc/bin/llvm-ar rcus" TARGET_STRIP="$tc/bin/llvm-strip" \
        TARGET_SYS=Linux BUILDMODE=static XCFLAGS=-fPIC
    make -C "$work/luajit" install PREFIX="$prefix"
    rm -f "$prefix"/lib/libluajit-5.1.so*
    touch "$work/luajit/.done"
fi

fetch dovi_tool https://github.com/quietvoid/dovi_tool.git libdovi-3.4.0
if [ ! -f "$work/dovi_tool/.done" ]; then
    (cd "$work/dovi_tool/dolby_vision" &&
        cargo cinstall --release --target $rtarget --prefix "$prefix" --libdir lib \
            --library-type staticlib)
    touch "$work/dovi_tool/.done"
fi

fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git n9.0.1 ffmpeg-mediacodec.patch
if [ ! -f "$work/ffmpeg/.done" ]; then
    mkdir -p "$work/ffmpeg/build"
    cd "$work/ffmpeg/build"
    ../configure --prefix="$prefix" --target-os=android --arch=$ffarch \
        --enable-cross-compile --cc="$cc" --cxx="$cc++" --ar="$tc/bin/llvm-ar" \
        --ranlib="$tc/bin/llvm-ranlib" --nm="$tc/bin/llvm-nm" --strip="$tc/bin/llvm-strip" \
        --sysroot="$tc/sysroot" --pkg-config=pkg-config --pkg-config-flags=--static \
        --extra-cflags="-I$prefix/include -fPIC" --extra-ldflags="-L$prefix/lib" \
        --enable-static --disable-shared --enable-pic \
        --disable-programs --disable-doc --disable-autodetect --disable-devices \
        --enable-version3 --enable-jni --enable-mediacodec --enable-network \
        --enable-mbedtls --enable-libxml2 --enable-libdav1d \
        --disable-encoders --enable-encoder=ac3,eac3,mjpeg,png \
        --disable-muxers --enable-muxer=spdif,image2,mjpeg \
        --enable-swresample --enable-swscale --enable-avfilter
    make -j"$jobs" install
    touch "$work/ffmpeg/.done"
    cd "$src"
fi

fetch freetype https://github.com/freetype/freetype.git 5c79d6cd1ac73d70a55f3d963fb568aa32f6d794
meson_dep freetype -Dtests=disabled -Dharfbuzz=disabled -Dbrotli=disabled \
    -Dbzip2=disabled -Dpng=disabled -Dzlib=disabled -Derror_strings=false

fetch fribidi https://github.com/fribidi/fribidi.git c928e4c77549de59e057f72bbbec1e38ce8b43bd
meson_dep fribidi -Ddocs=false -Dbin=false -Dtests=false

fetch harfbuzz https://github.com/harfbuzz/harfbuzz.git 64e2b53235aa33b5bad5442449a092b5729be66a
meson_dep harfbuzz -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
    -Dchafa=disabled -Dpng=disabled -Dzlib=disabled -Dicu=disabled \
    -Dfreetype=disabled -Dtests=disabled -Dintrospection=disabled -Ddocs=disabled \
    -Dutilities=disabled -Dbenchmark=disabled -Dgraphite2=disabled \
    -Dsubset=disabled -Draster=disabled -Dvector=disabled -Dgpu=disabled

fetch libass https://github.com/libass/libass.git b2fe9d8770678a7b5271387d38c20657ebf3429a \
    libass-android-fontprovider.patch
meson_dep libass -Dfontconfig=disabled -Ddirectwrite=disabled -Dcoretext=disabled \
    -Dlibunibreak=disabled -Dtest=disabled -Dcompare=disabled -Dprofile=disabled \
    -Dcheckasm=disabled -Dfuzz=disabled -Drequire-system-font-provider=false

fetch libplacebo https://github.com/haasn/libplacebo.git 3330a515d62139259c26239014f286e233bd3a5c
meson_dep libplacebo -Dtests=false -Ddemos=false -Dbench=false -Dfuzz=false \
    -Dvulkan=disabled -Dopengl=enabled -Dd3d11=disabled -Dshaderc=disabled \
    -Dglslang=disabled -Dlcms=disabled -Dlibdovi=disabled -Dxxhash=disabled \
    -Dunwind=disabled

static="libdovi.a:libdav1d.a:libxml2.a:libmbedtls.a:libmbedx509.a:libmbedcrypto.a:libluajit-5.1.a"
[ -f "$work/mpv/build.ninja" ] || meson setup "$work/mpv" "$src" \
    --cross-file "$work/cross.ini" --prefix "$prefix" --libdir lib \
    --buildtype release --default-library shared \
    -Dlibmpv=true -Dcplayer=false -Dgpl=true -Dbuild-date=false -Dtests=false \
    -Dlua=luajit -Djavascript=disabled -Dcplugins=disabled -Diconv=disabled \
    -Dlcms2=disabled -Dlibarchive=disabled -Dlibavdevice=disabled \
    -Dlibbluray=disabled -Ddvdnav=disabled -Dcdda=disabled -Dlibcurl=disabled \
    -Dlibdovi=enabled -Drubberband=disabled -Duchardet=disabled -Dvapoursynth=disabled \
    -Dzimg=disabled -Dzlib=disabled -Djpeg=disabled -Dsdl2-gamepad=disabled \
    -Dsdl2-audio=disabled -Dsdl2-video=disabled -Dalsa=disabled -Djack=disabled \
    -Dopenal=disabled -Dpipewire=disabled -Dpulse=disabled -Dsndio=disabled \
    -Doss-audio=disabled -Daaudio=disabled -Dopensles=disabled -Daudiotrack=enabled \
    -Dcaca=disabled -Ddrm=disabled -Degl=disabled -Degl-android=enabled -Dgl=enabled \
    -Dgl-x11=disabled -Dshaderc=disabled -Dsixel=disabled -Dspirv-cross=disabled \
    -Dvdpau=disabled -Dvaapi=disabled -Dvulkan=disabled -Dwayland=disabled \
    -Dx11=disabled -Dx11-clipboard=disabled -Dandroid-media-ndk=enabled \
    -Dcuda-hwaccel=disabled -Dcuda-interop=disabled -Dmanpage-build=disabled \
    -Dhtml-build=disabled -Dpdf-build=disabled \
    "-Dc_link_args=-lc++_shared -L$prefix/lib -Wl,--exclude-libs,$static -ldovi -ldav1d -lxml2 -lmbedtls -lmbedx509 -lmbedcrypto -lm"
meson compile -C "$work/mpv"

out=$work/out/jni/$abi
mkdir -p "$out" "$work/out/include/mpv"
cp "$work/mpv/libmpv.so" "$out/"
cp "$tc/sysroot/usr/lib/$triple/libc++_shared.so" "$out/"
cp "$src"/include/mpv/*.h "$work/out/include/mpv/"
cp -r "$prefix/include/libavcodec" "$work/out/include/"
echo "built $out/libmpv.so"
