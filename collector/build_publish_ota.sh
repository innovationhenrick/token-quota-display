#!/bin/sh
set -eu

# Compila o firmware atual e publica no coletor local. O ESP32 fará o download
# automaticamente na próxima janela de verificação OTA.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
FIRMWARE_DIR="$PROJECT_DIR/firmware"
CLI=${ARDUINO_CLI:-/opt/homebrew/bin/arduino-cli}
LIBRARIES_DIR=${ARDUINO_LIBRARIES:-$HOME/Documents/Arduino/libraries}
FQBN=${ARDUINO_FQBN:-'esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=min_spiffs,JTAGAdapter=default,UploadSpeed=921600'}

VERSION_CODE=$(sed -n 's/.*FIRMWARE_VERSION_CODE = \([0-9][0-9]*\)UL;.*/\1/p' "$FIRMWARE_DIR/token_quota_display.ino")
[ -n "$VERSION_CODE" ] || { echo "FIRMWARE_VERSION_CODE não encontrado" >&2; exit 1; }

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/token-quota-ota.XXXXXX")
STAGE="$WORK_DIR/token_quota_display"
BUILD="$WORK_DIR/out"
mkdir -p "$STAGE"
cp "$FIRMWARE_DIR/token_quota_display.ino" "$STAGE/token_quota_display.cpp"
cp "$FIRMWARE_DIR/arduino_entrypoint.ino" "$STAGE/token_quota_display.ino"
cp "$FIRMWARE_DIR/secrets.h" "$STAGE/secrets.h"

"$CLI" compile --build-path "$BUILD" \
  --build-property runtime.tools.ctags.path=/opt/homebrew/bin \
  --build-property runtime.tools.ctags-5.8-arduino11.path=/opt/homebrew/bin \
  --fqbn "$FQBN" --libraries "$LIBRARIES_DIR" "$STAGE"

"$SCRIPT_DIR/publish_ota.sh" "$BUILD/token_quota_display.ino.bin" "$VERSION_CODE"
