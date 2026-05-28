#!/bin/bash
# the following must be executed in env targeting arm64 (e.g. `vcvarsall x64_arm64`)

PREFIX=`realpath ./ARM64`

SOURCE=$1
if [[ ! -f "$SOURCE/libavcodec/avcodec.c" ]]; then
    echo "need path to ffmpeg sources"
    exit 1
fi
pushd $SOURCE > /dev/null

# workaround for wsl issue - the *.d files wind up with windows
# drive prefix in paths (unless patch is made to ffmpeg)
git clean -fX > /dev/null

if [[ -d $PREFIX ]]; then
    rm -rf $PREFIX
fi

./configure --extra-cflags='-MD -GS-' \
    --enable-gpl \
    --enable-version3 \
    --prefix=$PREFIX \
    --toolchain=msvc \
    --target-os=win64 \
    --arch=arm64 \
    --disable-asm \
    --enable-cross-compile \
    --disable-avdevice \
    --disable-programs \
    --disable-avfilter \
    --disable-postproc \
    --disable-doc \
    --disable-pthreads \
    --enable-w32threads \
    --disable-network \
    --disable-everything \
    --disable-encoders \
    --disable-muxers \
    --disable-hwaccels \
    --disable-parsers \
    --disable-protocols \
    --enable-static \
    --disable-shared \
    --enable-muxer=avi \
    --enable-encoder=ffv1 \
    --enable-encoder=mpeg4 \
    --enable-encoder=utvideo \
    --enable-protocol=file

make install

rm -rf $PREFIX/share
rm -rf $PREFIX/lib/pkgconfig

pushd $PREFIX/lib > /dev/null
for f in lib*\.a; do
    mv $f $(echo "$f" | sed -E 's/^lib(.*).a$/\1.lib/')
done
