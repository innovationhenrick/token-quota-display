#!/usr/bin/env python3
"""Exporta somente as cotas do Antigravity recebidas pela statusLine.

O Antigravity CLI envia um JSON completo para stdin da statusLine. Este
adaptador descarta prompts, e-mail e contexto e persiste apenas a cota
necessária ao painel físico.
"""

import json
import os
import sys
import tempfile
import time


OUTPUT_PATH = os.environ.get(
    "TOKEN_QUOTA_ANTIGRAVITY_QUOTA_PATH",
    os.path.expanduser(
        "~/Library/Application Support/TokenQuotaDisplay/antigravity-quota.json"
    ),
)


def main():
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError):
        return 0
    if not isinstance(payload, dict):
        return 0

    quota = payload.get("quota")
    if not isinstance(quota, dict):
        return 0

    # Preserve only quota buckets. This intentionally excludes transcript,
    # prompt, account email and context-window details.
    safe_quota = {}
    for key, value in quota.items():
        if not isinstance(key, str) or not isinstance(value, dict):
            continue
        safe = {}
        for field in (
            "remaining_fraction",
            "remaining_amount",
            "reset_time",
            "reset_in_seconds",
            "disabled",
        ):
            if field in value:
                safe[field] = value[field]
        if safe:
            safe_quota[key] = safe

    if not safe_quota:
        return 0

    output = {
        "source": "antigravity-statusline",
        "updated_epoch": int(time.time()),
        "quota": safe_quota,
    }
    directory = os.path.dirname(os.path.expanduser(OUTPUT_PATH))
    os.makedirs(directory, exist_ok=True)
    target = os.path.expanduser(OUTPUT_PATH)
    fd, temporary = tempfile.mkstemp(prefix="antigravity-quota-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(output, handle, separators=(",", ":"))
        os.replace(temporary, target)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass

    bucket = safe_quota.get("gemini-weekly") or safe_quota.get("gemini_weekly")
    if bucket and bucket.get("remaining_fraction") is not None:
        remaining = round(float(bucket["remaining_fraction"]) * 100)
        print(f"Gemini semanal: {remaining}% restante")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
