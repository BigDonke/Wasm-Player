#!/bin/bash
# Builds ../ff.js + ../ff.wasm: a minimal FFmpeg (demux, audio decode, AAC encode, MP4 mux)
# wrapped by src/ffcore.c. Run from anywhere. The first run downloads emsdk and the FFmpeg source.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(pwd)
# Renovate bumps these two lines, see renovate.json
# renovate: datasource=github-tags depName=emscripten-core/emsdk
EMSDK_VERSION=7.0.1
# renovate: datasource=github-tags depName=FFmpeg/FFmpeg
FFMPEG_VERSION=9.1
FFSRC="$ROOT/ffmpeg-$FFMPEG_VERSION"
PREFIX="$ROOT/ffmpeg-out-$FFMPEG_VERSION-emsdk-$EMSDK_VERSION"

if [ ! -d "$ROOT/emsdk" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$ROOT/emsdk"
fi
if [ ! -d "$ROOT/emsdk/upstream/emscripten" ] || ! grep -qx "$EMSDK_VERSION" "$ROOT/emsdk/.active-version" 2>/dev/null; then
  git -C "$ROOT/emsdk" pull --ff-only
  "$ROOT/emsdk/emsdk" install "$EMSDK_VERSION"
  "$ROOT/emsdk/emsdk" activate "$EMSDK_VERSION"
  echo "$EMSDK_VERSION" > "$ROOT/emsdk/.active-version"
fi
export EMSDK_QUIET=1
source "$ROOT/emsdk/emsdk_env.sh"

DEMUXERS=matroska,mov,mpegts,avi,flv,ogg,flac,wav,w64,aac,ac3,eac3,dts,dtshd,truehd,mlp,mp3
PARSERS=h264,hevc,av1,vp9,aac,aac_latm,ac3,dca,mlp,flac,opus,vorbis,mpegaudio,dvbsub,dvdsub
DECODERS=aac,aac_latm,ac3,eac3,dca,truehd,mlp,flac,opus,vorbis,mp3,mp2,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_f32le,pcm_u8,pcm_bluray,pcm_dvd,pcm_alaw,pcm_mulaw,pgssub,dvdsub,dvbsub
CFLAGS="-O3 -sUSE_ZLIB=1"

if [ ! -f "$PREFIX/lib/libavformat.a" ]; then
  if [ ! -d "$FFSRC" ]; then
    curl -fL "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" | tar -xJ -C "$ROOT"
  fi
  rm -rf "$ROOT/ffmpeg-obj" && mkdir -p "$ROOT/ffmpeg-obj" && cd "$ROOT/ffmpeg-obj"
  emconfigure "$FFSRC/configure" \
    --prefix="$PREFIX" --target-os=none --arch=x86_32 --enable-cross-compile \
    --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm \
    --disable-x86asm --disable-inline-asm --disable-asm --disable-stripping \
    --disable-programs --disable-doc --disable-debug --disable-runtime-cpudetect \
    --disable-autodetect --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-network --disable-everything \
    --disable-avdevice --disable-avfilter --disable-swscale \
    --enable-small --enable-zlib \
    --enable-demuxer=$DEMUXERS --enable-parser=$PARSERS --enable-decoder=$DECODERS \
    --enable-encoder=aac --enable-muxer=mp4 --enable-bsf=aac_adtstoasc,vp9_superframe \
    --extra-cflags="$CFLAGS" --extra-ldflags="-sUSE_ZLIB=1"
  emmake make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
  emmake make install
  cd "$ROOT"
fi

emcc -O3 src/ffcore.c -I"$PREFIX/include" -L"$PREFIX/lib" \
  -lavformat -lavcodec -lswresample -lavutil -sUSE_ZLIB=1 \
  -sMODULARIZE=1 -sEXPORT_NAME=FFCore -sENVIRONMENT=worker \
  -sSTACK_SIZE=1048576 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=2147483648 \
  -sFILESYSTEM=0 -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString -sINCOMING_MODULE_JS_API=wasmBinary \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -o "$ROOT/../ff.js"

ls -la "$ROOT/../ff.js" "$ROOT/../ff.wasm"
