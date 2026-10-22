#!/bin/sh
# Fetch Ubuntu 20.04 (Focal) Mosquitto 1.6.9 + OpenSSL 1.1 runtime
# packages for a selected target architecture.
# Usage:
#   ./scripts/fetch_mosquitto_ubuntu20.sh            # host architecture
#   ./scripts/fetch_mosquitto_ubuntu20.sh amd64      # x86_64 target
#   ./scripts/fetch_mosquitto_ubuntu20.sh arm64      # ARM64/aarch64 target
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_DIR="${TMPDIR:-/tmp}/lljs_mosquitto_$$"
MOSQUITTO_VERSION="1.6.9-1"
OPENSSL_VERSION="1.1.1f-1ubuntu2.24"

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT INT TERM

TARGET_ARCH="${1:-}"
if [ -z "$TARGET_ARCH" ]; then
    TARGET_ARCH=$(dpkg --print-architecture 2>/dev/null || true)
fi
case "$TARGET_ARCH" in
    amd64|x86_64) TARGET_ARCH=amd64; ARCH_DIR=x86_64; BASE_MQ="https://archive.ubuntu.com/ubuntu/pool/universe/m/mosquitto"; BASE_SSL="https://archive.ubuntu.com/ubuntu/pool/main/o/openssl" ;;
    arm64|aarch64) TARGET_ARCH=arm64; ARCH_DIR=aarch64; BASE_MQ="https://ports.ubuntu.com/ubuntu-ports/pool/universe/m/mosquitto"; BASE_SSL="https://ports.ubuntu.com/ubuntu-ports/pool/main/o/openssl" ;;
    *) echo "Unsupported target architecture: $TARGET_ARCH (expected amd64/x86_64 or arm64/aarch64)" >&2; exit 1 ;;
esac

LIB_DIR="$ROOT/lib/$ARCH_DIR"
BIN_DIR="$ROOT/bin/$ARCH_DIR"
INC_DIR="$ROOT/include"
mkdir -p "$LIB_DIR" "$BIN_DIR" "$INC_DIR" "$TMP_DIR"

command -v curl >/dev/null 2>&1 || { echo "curl is required" >&2; exit 1; }
command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb is required" >&2; exit 1; }

MQ_DEV="libmosquitto-dev_${MOSQUITTO_VERSION}_${TARGET_ARCH}.deb"
MQ_RUNTIME="libmosquitto1_${MOSQUITTO_VERSION}_${TARGET_ARCH}.deb"
MQ_BROKER="mosquitto_${MOSQUITTO_VERSION}_${TARGET_ARCH}.deb"
SSL_RUNTIME="libssl1.1_${OPENSSL_VERSION}_${TARGET_ARCH}.deb"

fetch() {
    name="$1"; base="$2"
    echo "Downloading $name"
    curl -fL --retry 3 --retry-delay 1 -o "$TMP_DIR/$name" "$base/$name"
}

fetch "$MQ_RUNTIME" "$BASE_MQ"
fetch "$MQ_BROKER" "$BASE_MQ"
fetch "$MQ_DEV" "$BASE_MQ"
fetch "$SSL_RUNTIME" "$BASE_SSL"

dpkg-deb -x "$TMP_DIR/$MQ_RUNTIME" "$TMP_DIR/runtime"
dpkg-deb -x "$TMP_DIR/$MQ_DEV" "$TMP_DIR/dev"
dpkg-deb -x "$TMP_DIR/$MQ_BROKER" "$TMP_DIR/broker"
dpkg-deb -x "$TMP_DIR/$SSL_RUNTIME" "$TMP_DIR/openssl"

RUNTIME_LIB=$(find "$TMP_DIR/runtime" -type f -name 'libmosquitto.so.1.6.9' | head -n 1)
[ -n "$RUNTIME_LIB" ] && [ -f "$RUNTIME_LIB" ] || { echo "libmosquitto.so.1.6.9 not found" >&2; exit 1; }
cp -f "$RUNTIME_LIB" "$LIB_DIR/libmosquitto.so.1.6.9"
ln -sfn libmosquitto.so.1.6.9 "$LIB_DIR/libmosquitto.so.1"
ln -sfn libmosquitto.so.1.6.9 "$LIB_DIR/libmosquitto.so"

SSL_LIB=$(find "$TMP_DIR/openssl" -type f -name 'libssl.so.1.1' | head -n 1)
CRYPTO_LIB=$(find "$TMP_DIR/openssl" -type f -name 'libcrypto.so.1.1' | head -n 1)
[ -n "$SSL_LIB" ] && [ -f "$SSL_LIB" ] || { echo "libssl.so.1.1 not found in $SSL_RUNTIME" >&2; exit 1; }
[ -n "$CRYPTO_LIB" ] && [ -f "$CRYPTO_LIB" ] || { echo "libcrypto.so.1.1 not found in $SSL_RUNTIME" >&2; exit 1; }
cp -f "$SSL_LIB" "$LIB_DIR/libssl.so.1.1"
cp -f "$CRYPTO_LIB" "$LIB_DIR/libcrypto.so.1.1"
# OpenSSL runtime packages do not provide the development symlinks. These local
# links are only for the linker; the runtime SONAME remains libssl.so.1.1/
# libcrypto.so.1.1.
ln -sfn libssl.so.1.1 "$LIB_DIR/libssl.so"
ln -sfn libcrypto.so.1.1 "$LIB_DIR/libcrypto.so"

BROKER_BIN=$(find "$TMP_DIR/broker" -type f -path '*/usr/sbin/mosquitto' | head -n 1)
[ -n "$BROKER_BIN" ] && [ -f "$BROKER_BIN" ] || { echo "mosquitto broker executable not found" >&2; exit 1; }
cp -f "$BROKER_BIN" "$BIN_DIR/mosquitto"
chmod +x "$BIN_DIR/mosquitto"

[ -f "$TMP_DIR/dev/usr/include/mosquitto.h" ] || { echo "mosquitto.h not found" >&2; exit 1; }
cp -f "$TMP_DIR/dev/usr/include/mosquitto.h" "$INC_DIR/mosquitto.h"

echo ""
echo "Mosquitto $MOSQUITTO_VERSION + OpenSSL $OPENSSL_VERSION prepared for $TARGET_ARCH ($ARCH_DIR)"
echo "  Broker:       $BIN_DIR/mosquitto"
echo "  Mosquitto:    $LIB_DIR/libmosquitto.so.1.6.9"
echo "  OpenSSL SSL:  $LIB_DIR/libssl.so.1.1"
echo "  OpenSSL CR:   $LIB_DIR/libcrypto.so.1.1"
echo "  Header:       $INC_DIR/mosquitto.h"
