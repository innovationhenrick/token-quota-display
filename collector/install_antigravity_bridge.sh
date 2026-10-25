#!/bin/sh
set -eu

# Instala a ponte oficial da statusLine sem substituir uma configuração
# personalizada já existente. O backup fica no mesmo diretório do settings.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
AG_DIR="$HOME/.gemini/antigravity-cli"
SETTINGS="$AG_DIR/settings.json"
BRIDGE="$AG_DIR/token-quota-statusline.py"

mkdir -p "$AG_DIR"
cp "$SCRIPT_DIR/antigravity_statusline.py" "$BRIDGE"
chmod 755 "$BRIDGE"

if [ ! -f "$SETTINGS" ]; then
  printf '%s\n' '{}' > "$SETTINGS"
fi

BACKUP="$SETTINGS.bak.$(date +%Y%m%d%H%M%S)"
cp "$SETTINGS" "$BACKUP"

SETTINGS_PATH="$SETTINGS" BRIDGE_PATH="$BRIDGE" /usr/bin/python3 - <<'PY'
import json
import os

settings_path = os.environ["SETTINGS_PATH"]
bridge_path = os.environ["BRIDGE_PATH"]
with open(settings_path, "r", encoding="utf-8") as handle:
    settings = json.load(handle)
if not isinstance(settings, dict):
    raise SystemExit("settings.json não contém um objeto JSON")

existing = settings.get("statusLine")
if existing and existing.get("command") != bridge_path:
    raise SystemExit(
        "statusLine já configurada; backup preservado, integração manual necessária"
    )

settings["statusLine"] = {
    "type": "command",
    "command": "/usr/bin/python3 " + repr(bridge_path),
    "enabled": True,
    "stack_with_default": True,
}
temporary = settings_path + ".tmp"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(settings, handle, indent=2, ensure_ascii=False)
    handle.write("\n")
os.replace(temporary, settings_path)
PY

echo "Ponte Antigravity instalada: $BRIDGE"
echo "Backup das configurações: $BACKUP"
