#!/bin/sh
export NDK=/home/tibo/ndk/25.2.9519653
# Only choose one of these, depending on your development platform
export HOST_TAG=linux-x86_64
# export HOST_TAG=darwin-x86_64
# export HOST_TAG=windows
# export HOST_TAG=windows-x86_64
export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/$HOST_TAG

# Only choose one of these, depending on your target device
#export TARGET=aarch64-linux-android # arm64-v8a
#export TARGET=arm-linux-androideabi # armeabi-v7a
#export TARGET=i686-linux-android # x86
export TARGET=x86_64-linux-android # x86_64

# Set this to your minSdkVersion.
export API=19

# Configure and build.
export AR=$TOOLCHAIN/bin/$TARGET-ar
export AS=$TOOLCHAIN/bin/$TARGET-as

if [ "$TARGET" = "arm-linux-androideabi" ]; then
export CC=$TOOLCHAIN/bin/armv7a-linux-androideabi$API-clang
export CXX=$TOOLCHAIN/bin/armv7a-linux-androideabi$API-clang++
else
export CC=$TOOLCHAIN/bin/$TARGET$API-clang
export CXX=$TOOLCHAIN/bin/$TARGET$API-clang++
fi

#We are not setting CPP C preprocessor environment variable, "$CC -E" invokes the clang C preprocessor
export LD=$TOOLCHAIN/bin/$TARGET-ld
export RANLIB=$TOOLCHAIN/bin/$TARGET-ranlib
export STRIP=$TOOLCHAIN/bin/$TARGET-strip

make clean && rm -r `pwd`/install
./configure \
	--host=$TARGET \
	--prefix=`pwd`/install \
	--with-jpeg-include-dir=`pwd`/jpeg-9d/include \
	--with-jpeg-lib-dir=`pwd`/jpeg-9d/lib/x86_64 \
	&& make && make install
