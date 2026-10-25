#!/bin/sh
set -eu

LABEL="com.rick.quotadisplay"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNTIME_DIR="$HOME/Library/Application Support/TokenQuotaDisplay"
AGENT_DIR="$HOME/Library/LaunchAgents"
PLIST_PATH="$AGENT_DIR/$LABEL.plist"

mkdir -p "$RUNTIME_DIR" "$AGENT_DIR"
cp "$SCRIPT_DIR/quota_server.py" "$RUNTIME_DIR/quota_server.py"
cp "$SCRIPT_DIR/limits.json" "$RUNTIME_DIR/limits.json"

# O LaunchAgent não consegue ler projetos dentro do Google Drive em algumas
# versões do macOS. A cópia de runtime em Application Support evita esse TCC.
escaped_runtime=$(printf '%s' "$RUNTIME_DIR" | sed 's/[&|]/\\&/g')
sed "s|__RUNTIME_DIR__|$escaped_runtime|g" \
  "$SCRIPT_DIR/com.rick.quotadisplay.plist" > "$PLIST_PATH"
plutil -lint "$PLIST_PATH" >/dev/null

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
# O launchd pode manter o processo anterior por alguns instantes após bootout.
# Sem essa pausa, um bootstrap imediato falha intermitentemente com erro 5.
sleep 1
launchctl bootstrap "gui/$(id -u)" "$PLIST_PATH"
launchctl kickstart -k "gui/$(id -u)/$LABEL"

echo "Coletor instalado e ativo: http://localhost:8787/quota"
