#!/bin/sh
set -eu

# Publica um binário já compilado de forma atômica para o coletor local.
# O ESP32 só vê a nova versão depois que o binário e o manifesto estão prontos.
if [ "$#" -ne 2 ]; then
  echo "uso: $0 BINARIO VERSION_CODE" >&2
  exit 2
fi

BINARY=$1
VERSION_CODE=$2
RUNTIME_DIR=${TOKEN_QUOTA_FIRMWARE_DIR:-"$HOME/Library/Application Support/TokenQuotaDisplay/firmware"}
TARGET="$RUNTIME_DIR/token_quota_display.bin"
MANIFEST="$RUNTIME_DIR/manifest.json"

case "$VERSION_CODE" in
  *[!0-9]*|"") echo "VERSION_CODE deve ser numerico" >&2; exit 2 ;;
esac
[ -f "$BINARY" ] || { echo "binario nao encontrado: $BINARY" >&2; exit 1; }

mkdir -p "$RUNTIME_DIR"
size=$(stat -f '%z' "$BINARY")
md5=$(md5 -q "$BINARY")
binary_tmp="$TARGET.tmp.$$"
manifest_tmp="$MANIFEST.tmp.$$"
cp "$BINARY" "$binary_tmp"
mv "$binary_tmp" "$TARGET"
printf '{"version_code":%s,"size":%s,"md5":"%s"}\n' \
  "$VERSION_CODE" "$size" "$md5" > "$manifest_tmp"
mv "$manifest_tmp" "$MANIFEST"
echo "OTA publicado: versao $VERSION_CODE, $size bytes"
