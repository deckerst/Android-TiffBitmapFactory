#!/bin/sh

# cf https://developer.android.com/ndk/guides/other_build_systems

export NDK=/home/tibo/Android/Sdk/ndk/28.0.12916984
export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64

# Only choose one of these, depending on your target device
#export TARGET=aarch64-linux-android # arm64-v8a
#export TARGET=armv7a-linux-androideabi # armeabi-v7a
#export TARGET=i686-linux-android # x86
export TARGET=x86_64-linux-android # x86_64

# Set this to your minSdkVersion.
export API=21

# Configure and build.
export AR=$TOOLCHAIN/bin/llvm-ar
export CC="$TOOLCHAIN/bin/clang --target=$TARGET$API"
export AS=$CC
export CXX="$TOOLCHAIN/bin/clang++ --target=$TARGET$API"
export LD=$TOOLCHAIN/bin/ld
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib
export STRIP=$TOOLCHAIN/bin/llvm-strip

make clean && rm -r `pwd`/install
./configure \
	--host=$TARGET \
	--prefix=`pwd`/install \
	--with-jpeg-include-dir=`pwd`/jpeg-9d/include \
	--with-jpeg-lib-dir=`pwd`/jpeg-9d/lib/x86_64 \
	&& make && make install
