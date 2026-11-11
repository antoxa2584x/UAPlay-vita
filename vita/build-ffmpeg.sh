#!/bin/sh
# Builds the FFmpeg subset used by the Vita player: HLS/HTTPS input,
# MPEG-TS/MP4 demuxing, H.264 (software fallback) and AAC/MP3/AC-3 audio.
# The vdpm "ffmpeg" package has no network protocols, hence this script.
# Usage: VITASDK=/usr/local/vitasdk sh vita/build-ffmpeg.sh [prefix]
set -eu
: "${VITASDK:?Set VITASDK}"
PREFIX=${1:-$HOME/uaplay-vita-deps}
VERSION=7.1
WORK=${TMPDIR:-/tmp}/uaplay-ffmpeg
export PATH="$VITASDK/bin:$PATH"
mkdir -p "$WORK"
cd "$WORK"
[ -f ffmpeg-$VERSION.tar.xz ] || curl -fL -o ffmpeg-$VERSION.tar.xz https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz
rm -rf ffmpeg-$VERSION
tar xf ffmpeg-$VERSION.tar.xz
cd ffmpeg-$VERSION
PKG_CONFIG_LIBDIR=$VITASDK/arm-vita-eabi/lib/pkgconfig ./configure --prefix="$PREFIX" \
  --enable-cross-compile --cross-prefix=arm-vita-eabi- --pkg-config=pkg-config \
  --disable-shared --enable-static --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2 \
  --arch=armv7-a --cpu=cortex-a9 --enable-neon --target-os=none \
  --disable-programs --disable-doc --disable-debug --disable-everything \
  --enable-network --enable-openssl --enable-pthreads \
  --enable-protocol=file,http,https,tcp,tls,hls,crypto,data \
  --enable-demuxer=hls,mpegts,mov,aac,mp3,matroska,webvtt \
  --enable-decoder=aac,aac_latm,mp3,ac3,eac3,h264 \
  --enable-parser=h264,aac,aac_latm,ac3,mpegaudio \
  --enable-bsf=h264_mp4toannexb,aac_adtstoasc \
  --enable-swresample --enable-swscale \
  --disable-bzlib --disable-iconv --disable-lzma --disable-sdl2 --disable-xlib --disable-securetransport \
  --extra-cflags="-std=gnu11 -O2 -ftree-vectorize -fomit-frame-pointer -D_BSD_SOURCE -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -I$VITASDK/arm-vita-eabi/include" \
  --extra-ldflags="-L$VITASDK/arm-vita-eabi/lib" --extra-libs="-lssl -lcrypto -lz -lpthread"
make -j"$(nproc)"
make install
