#!/bin/bash

#writed by jh.ai 2013-7-8 10:11:32

INSTALL_PREFIX=./out/noGPLlinux/

mkdir -p $INSTALL_PREFIX;
./configure \
    --disable-all \
    --prefix=$INSTALL_PREFIX \
    --enable-version3 \
    --enable-nonfree \
    --enable-static \
    --enable-shared \
    --enable-pic \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --enable-cross-compile \
    --cross-prefix=arm-none-linux-gnueabi- \
    --target-os=linux \
    --disable-runtime-cpudetect \
    --arch=arm \
    --cpu=cortex-a8 \
    --enable-neon \
    --enable-safe-bitstream-reader \
    --enable-memalign-hack \
    --enable-asm \
    --enable-vfp \
    --disable-debug \
    --disable-zlib \
    --disable-ffmpeg \
    --disable-ffplay \
    --disable-ffprobe \
    --disable-ffserver \
    --disable-avdevice \
    --disable-postproc \
    --disable-avfilter \
    --disable-pthreads \
    --disable-network \
    --disable-encoders \
    --disable-decoders \
    --disable-parsers \
    --disable-symver \
    --enable-avutil \
    --enable-avcodec \
    --enable-avformat \
    --enable-swresample \
    --enable-swscale \
    --enable-decoder=eamad \
    --enable-decoder=aac \
    --enable-decoder=aac_latm \
    --enable-decoder=dca \
    --enable-decoder=ac3 \
    --enable-decoder=ape \
    --enable-decoder=wmav1 \
    --enable-decoder=wmav2 \
    --enable-decoder=wmapro \
    --enable-decoder=wmalossless \
    --enable-decoder=wmavoice \
    --enable-decoder=atrac1 \
    --enable-decoder=atrac3 \
    --enable-decoder=flac \
    --enable-decoder=dirac \
    --enable-decoder=mp1 \
    --enable-decoder=mp2 \
    --enable-decoder=mp3 \
    --enable-decoder=mp3adu \
    --enable-decoder=mp3on4 \
    --enable-decoder=mp1float \
    --enable-decoder=mp2float \
    --enable-decoder=mp3float \
    --enable-decoder=mp3adufloat \
    --enable-decoder=mp3on4float \
    --enable-decoder=cook \
    --enable-decoder=amrnb \
    --enable-decoder=amrwb \
    --enable-decoder=snow \
    --enable-decoder=vorbis \
    --enable-decoder=ra_144 \
    --enable-decoder=ra_288 \
    --enable-decoder=amrwb \
    --enable-decoder=alac \
    --enable-decoder=ws_snd1 \
    --enable-decoder=eightsvx_fib \
    --enable-decoder=eightsvx_exp \
    --enable-decoder=pcm_s8_planar \
    --enable-decoder=eac3 \
    --enable-decoder=adpcm_4xm \
    --enable-decoder=adpcm_ct \
    --enable-decoder=adpcm_ea \
    --enable-decoder=adpcm_ea_maxis_xa \
    --enable-decoder=adpcm_ea_r1 \
    --enable-decoder=adpcm_ea_r2 \
    --enable-decoder=adpcm_ea_r3 \
    --enable-decoder=adpcm_ea_xas \
    --enable-decoder=adpcm_ima_amv \
    --enable-decoder=adpcm_ima_apc \
    --enable-decoder=adpcm_ima_dk3 \
    --enable-decoder=adpcm_ima_dk4 \
    --enable-decoder=adpcm_ima_ea_eacs \
    --enable-decoder=adpcm_ima_ea_sead \
    --enable-decoder=adpcm_ima_iss \
    --enable-decoder=adpcm_ima_qt \
    --enable-decoder=adpcm_ima_smjpeg \
    --enable-decoder=adpcm_ima_wav \
    --enable-decoder=adpcm_ima_ws \
    --enable-decoder=adpcm_ms \
    --enable-decoder=adpcm_sbpro_2 \
    --enable-decoder=adpcm_sbpro_3 \
    --enable-decoder=adpcm_sbpro_4 \
    --enable-decoder=adpcm_swf \
    --enable-decoder=adpcm_thp \
    --enable-decoder=adpcm_xa \
    --enable-decoder=adpcm_yamaha \
    --enable-decoder=adpcm_adx \
    --enable-decoder=als \
    --enable-decoder=binkaudio_rdft \
    --enable-decoder=binkaudio_dct \
    --enable-decoder=interplay_dpcm \
    --enable-decoder=roq_dpcm \
    --enable-decoder=sol_dpcm \
    --enable-decoder=ffwavesynth \
    --enable-decoder=adpcm_g722 \
    --enable-decoder=g723_1 \
    --enable-decoder=adpcm_g726 \
    --enable-decoder=g729 \
    --enable-decoder=gsm \
    --enable-decoder=gsm_ms \
    --enable-decoder=imc \
    --enable-decoder=pcm_bluray \
    --enable-decoder=pcm_alaw \
    --enable-decoder=pcm_dvd \
    --enable-decoder=pcm_f32be \
    --enable-decoder=pcm_f32le \
    --enable-decoder=pcm_f64be \
    --enable-decoder=pcm_f64le \
    --enable-decoder=pcm_lxf \
    --enable-decoder=pcm_mulaw \
    --enable-decoder=pcm_s8 \
    --enable-decoder=pcm_s16be \
    --enable-decoder=pcm_s16le \
    --enable-decoder=pcm_s16le_planar \
    --enable-decoder=pcm_s24be \
    --enable-decoder=pcm_s24daud \
    --enable-decoder=pcm_s24le \
    --enable-decoder=pcm_s32be \
    --enable-decoder=pcm_s32le \
    --enable-decoder=pcm_u8 \
    --enable-decoder=pcm_u16be \
    --enable-decoder=pcm_u16le \
    --enable-decoder=pcm_u24be \
    --enable-decoder=pcm_u24le \
    --enable-decoder=pcm_u32be \
    --enable-decoder=pcm_u32le \
    --enable-decoder=pcm_zork \
    --enable-decoder=qcelp \
    --enable-decoder=qdm2 \
    --enable-decoder=s302m \
    --enable-decoder=shorten \
    --enable-decoder=sipr \
    --enable-decoder=sonic \
    --enable-decoder=truespeech \
    --enable-decoder=tta \
    --enable-decoder=twinvq \
    --enable-decoder=wavpack \
    --enable-decoder=ws_snd1 \
    --enable-decoder=mlp \
    --enable-decoder=truehd \
    --enable-parser=dca \
    --enable-parser=cook \
    --extra-cflags="-mfloat-abi=softfp -mfpu=neon" \
  
