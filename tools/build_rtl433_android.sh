#!/bin/sh
# Compile rtl_433 (https://github.com/merbanan/rtl_433, GPL-2.0) pour Android avec le
# NDK, en exécutable statique sans pilote SDR : il ne sert qu'à relire les fichiers
# .ook produits par l'onglet ISM.
# Threads désactivés : la libc Android (bionic) n'a pas pthread_cancel, utilisé par le
# serveur rtl_tcp de rtl_433 que nous n'employons pas. Résultat : app/UVLink.Android/native/<abi>/librtl_433.so
# (nom en lib*.so pour être empaqueté dans lib/<abi>/ de l'APK et extrait sur disque).
#   tools/build_rtl433_android.sh [dossier_source_rtl_433]
# Sans argument, clone le tag 23.11 (même version que le paquet Debian/Ubuntu) dans build/.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
NDK=${ANDROID_NDK_HOME:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* | sort -V | tail -1)}
SRC=${1:-$HERE/build/rtl_433}
TAG=23.11
if [ ! -d "$SRC" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 --branch $TAG https://github.com/merbanan/rtl_433 "$SRC"
fi
for ABI in arm64-v8a armeabi-v7a x86_64; do
    B=$HERE/build/rtl_433-$ABI
    # 32 bits : fseeko (mongoose.c) n'existe qu'à partir de l'API 24 (Android 7)
    PLAT=23; [ $ABI = armeabi-v7a ] && PLAT=24
    cmake -S "$SRC" -B "$B" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$PLAT -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF -DENABLE_THREADS=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static"
    ninja -C "$B" rtl_433
    mkdir -p "$HERE/app/UVLink.Android/native/$ABI"
    "$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o "$HERE/app/UVLink.Android/native/$ABI/librtl_433.so" "$B/src/rtl_433"
    ls -l "$HERE/app/UVLink.Android/native/$ABI/librtl_433.so"
done
