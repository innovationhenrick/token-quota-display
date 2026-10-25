#!/usr/bin/env python3
"""
Servidor de cotas do Claude Code, Codex CLI, Gemini/Antigravity e Grok.

Expõe GET /quota em http://0.0.0.0:8787 com o contrato:
{"claude":{...}, "codex":{...},
 "gemini":{"week_pct":int,"week_remaining_pct":int|null,
            "week_reset_min":int|null,"quota_kind":"weekly"},
 "grok":{"week_pct":int,"week_remaining_pct":int|null,
          "week_reset_min":int|null,"quota_kind":"weekly"},
 "updated":epoch_seconds}

Usa apenas stdlib. Não lê nem grava conteúdo de prompt: apenas
contadores de tokens, timestamps e campos de rate_limits.
"""

import base64
import glob
import json
import math
import os
import re
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --------------------------------------------------------------------------
# Configuração
# --------------------------------------------------------------------------

HOST = "0.0.0.0"
PORT = 8787
FIRMWARE_DIR = os.environ.get(
    "TOKEN_QUOTA_FIRMWARE_DIR",
    os.path.expanduser("~/Library/Application Support/TokenQuotaDisplay/firmware"),
)
FIRMWARE_MANIFEST_PATH = os.path.join(FIRMWARE_DIR, "manifest.json")
FIRMWARE_BINARY_PATH = os.path.join(FIRMWARE_DIR, "token_quota_display.bin")
CACHE_SECONDS = 5
API_TIMEOUT_SECONDS = 10
CLAUDE_API_CACHE_SECONDS = 60
CODEX_API_CACHE_SECONDS = 5
EXTERNAL_QUOTA_MAX_AGE_SECONDS = 20 * 60
ANTIGRAVITY_REFRESH_INTERVAL_SECONDS = 5 * 60
GROK_REFRESH_INTERVAL_SECONDS = 5 * 60

CLAUDE_PROJECTS_GLOB = os.path.expanduser("~/.claude/projects/*/*.jsonl")
CODEX_SESSIONS_GLOB = os.path.expanduser("~/.codex/sessions/**/*.jsonl")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LIMITS_PATH = os.path.join(SCRIPT_DIR, "limits.json")
HISTORY_PATH = os.environ.get(
    "TOKEN_QUOTA_HISTORY_PATH",
    os.path.expanduser("~/Library/Application Support/TokenQuotaDisplay/history.json"),
)
CLAUDE_API_SNAPSHOT_PATH = os.environ.get(
    "TOKEN_QUOTA_CLAUDE_SNAPSHOT_PATH",
    os.path.expanduser("~/Library/Application Support/TokenQuotaDisplay/claude-api-snapshot.json"),
)
ANTIGRAVITY_QUOTA_PATH = os.environ.get(
    "TOKEN_QUOTA_ANTIGRAVITY_QUOTA_PATH",
    os.path.expanduser("~/Library/Application Support/TokenQuotaDisplay/antigravity-quota.json"),
)
GROK_QUOTA_PATH = os.environ.get(
    "TOKEN_QUOTA_GROK_QUOTA_PATH",
    os.path.expanduser("~/Library/Application Support/TokenQuotaDisplay/grok-quota.json"),
)
ANTIGRAVITY_BIN = os.environ.get(
    "TOKEN_QUOTA_ANTIGRAVITY_BIN",
    os.path.expanduser("~/.local/bin/agy"),
)
GROK_BIN = os.environ.get(
    "TOKEN_QUOTA_GROK_BIN",
    os.path.expanduser("~/.local/bin/grok"),
)
GROK_AUTH_PATH = os.environ.get(
    "TOKEN_QUOTA_GROK_AUTH_PATH",
    os.path.expanduser("~/.grok/auth.json"),
)
GROK_BOT_PERSISTENCE_PATH = os.environ.get(
    "TOKEN_QUOTA_GROK_BOT_PERSISTENCE_PATH",
    os.path.expanduser("~/Library/Application Support/Grok Bot/sand-client-persistence"),
)
GROK_BOT_SLOT_KEY = "sand.client.slice.client-meta.account-slot"
GROK_BOT_WEEKLY_SUFFIX = ".weekly-usage.cache"
GROK_CREDITS_URL = "https://cli-chat-proxy.grok.com/v1/billing?format=credits"
GROK_TOKEN_URL = "https://auth.x.ai/oauth2/token"

_antigravity_refresh_lock = threading.Lock()
_antigravity_refresh_started = 0.0
_antigravity_refresh_running = False
_grok_refresh_lock = threading.Lock()
_grok_refresh_started = 0.0
_grok_refresh_running = False
_grok_api_cache = (0.0, None)
_grok_renewed_token = None

_DEFAULT_LIMITS = {
    "claude": {
        "session_window_hours": 5,
        "session_token_limit": 2_000_000,
        "week_window_days": 7,
        "week_token_limit": 20_000_000,
    }
}


# --------------------------------------------------------------------------
# Utilidades de tempo
# --------------------------------------------------------------------------

def now_utc():
    """Instante atual, sempre timezone-aware em UTC."""
    return datetime.now(timezone.utc)


def parse_iso_ts(raw):
    """Converte timestamp ISO 8601 (com 'Z') em datetime UTC. Retorna None se inválido."""
    if not isinstance(raw, str) or not raw:
        return None
    try:
        return datetime.fromisoformat(raw.replace("Z", "+00:00"))
    except (ValueError, TypeError):
        return None


def clamp_pct(value):
    """Garante percentual inteiro entre 0 e 100."""
    try:
        v = int(round(value))
    except (TypeError, ValueError):
        return 0
    return max(0, min(100, v))


def minutes_until(reset_at, now):
    """Minutos inteiros até um datetime/epoch de reset; None se indisponível."""
    if reset_at is None:
        return None
    try:
        if isinstance(reset_at, datetime):
            reset_dt = reset_at
        else:
            reset_dt = datetime.fromtimestamp(reset_at, tz=timezone.utc)
        return max(0, int((reset_dt - now).total_seconds() // 60))
    except (OSError, OverflowError, ValueError, TypeError):
        return None


def future_minutes_until(reset_at, now):
    """Como minutes_until, mas rejeita timestamps que já venceram."""
    if reset_at is None or reset_at <= now:
        return None
    return minutes_until(reset_at, now)


def roll_reset_forward(reset_at, now, window_minutes):
    """Avança um marco expirado por janelas fixas até o próximo reset."""
    if reset_at is None:
        return None
    while reset_at <= now:
        reset_at += timedelta(minutes=window_minutes)
    return reset_at


def _iso_or_epoch_to_datetime(value):
    if isinstance(value, str):
        return parse_iso_ts(value)
    try:
        return datetime.fromtimestamp(float(value), tz=timezone.utc)
    except (OSError, OverflowError, ValueError, TypeError):
        return None


def _window_is_live(reset_at, now):
    reset_dt = _iso_or_epoch_to_datetime(reset_at)
    return reset_dt is not None and reset_dt > now


def _http_json(url, headers):
    request = urllib.request.Request(url, headers=headers, method="GET")
    with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
        return json.load(response)


def _read_json_file(path):
    with open(os.path.expanduser(path), "r", encoding="utf-8") as fh:
        return json.load(fh)


def _write_json_file(path, value):
    """Grava metadados de cota de forma atômica, sem prompts ou credenciais."""
    expanded = os.path.expanduser(path)
    os.makedirs(os.path.dirname(expanded), exist_ok=True)
    temporary = expanded + ".tmp"
    with open(temporary, "w", encoding="utf-8") as fh:
        json.dump(value, fh)
    os.replace(temporary, expanded)


def _external_quota_unavailable(source):
    """Contrato estável para uma fonte semanal ainda não conectada."""
    return {
        "session_pct": 0,
        "week_pct": 0,
        "session_reset_min": None,
        "week_reset_min": None,
        "reset_min": None,
        "week_remaining_pct": None,
        "week_used_pct": None,
        "quota_kind": "weekly",
        "source": source,
        "stale": True,
        "available": False,
        "observed_epoch": None,
    }


def _coerce_fraction(value):
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    if 0 <= value <= 1:
        return value
    if 0 <= value <= 100:
        return value / 100.0
    return None


def _coerce_epoch(value, now):
    """Converte reset_time/epoch para epoch UTC, aceitando ISO e segundos."""
    parsed = _iso_or_epoch_to_datetime(value)
    if parsed is not None:
        return int(parsed.timestamp())
    try:
        seconds = int(float(value))
    except (TypeError, ValueError):
        return None
    if seconds < 0:
        return None
    # Antigravity fornece reset_in_seconds, que é relativo; timestamps Unix
    # atuais são grandes o bastante para serem distinguidos com segurança.
    if seconds < 10 * 365 * 24 * 3600:
        return int(now.timestamp()) + seconds
    return seconds


def _read_external_weekly_quota(path, provider, bucket_id=None, now=None):
    """Lê um snapshot local sem assumir que ele contém prompts ou credenciais.

    Gemini/Antigravity é escrito pela statusLine oficial. Grok usa o mesmo
    contrato de snapshot para que possamos trocar a fonte sem alterar o
    firmware. O percentual canônico do servidor continua sendo o usado.
    """
    if now is None:
        now = now_utc()
    try:
        payload = _read_json_file(path)
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return _external_quota_unavailable(provider + ":aguardando")
    if not isinstance(payload, dict):
        return _external_quota_unavailable(provider + ":invalido")

    observed = payload.get("updated_epoch")
    try:
        observed = int(float(observed))
    except (TypeError, ValueError):
        try:
            observed = int(os.path.getmtime(os.path.expanduser(path)))
        except OSError:
            observed = None

    age = None if observed is None else max(0, int(now.timestamp()) - observed)
    stale = age is None or age > EXTERNAL_QUOTA_MAX_AGE_SECONDS

    bucket = payload
    if bucket_id:
        quota = payload.get("quota")
        if isinstance(quota, dict):
            bucket = quota.get(bucket_id) or quota.get(bucket_id.replace("-", "_")) or {}
    elif isinstance(payload.get("weekly"), dict):
        bucket = payload["weekly"]

    if not isinstance(bucket, dict):
        return _external_quota_unavailable(provider + ":invalido")

    remaining_fraction = _coerce_fraction(
        bucket.get(
            "remaining_fraction",
            bucket.get("remaining_pct", bucket.get("remaining")),
        )
    )
    used_fraction = _coerce_fraction(
        bucket.get("used_fraction", bucket.get("used_pct", bucket.get("used")))
    )
    if remaining_fraction is None and used_fraction is not None:
        remaining_fraction = max(0.0, min(1.0, 1.0 - used_fraction))
    if used_fraction is None and remaining_fraction is not None:
        used_fraction = max(0.0, min(1.0, 1.0 - remaining_fraction))

    if remaining_fraction is None:
        # O Grok CLI informa tokens persistidos por sessão, mas não informa o
        # teto semanal do plano. Mantemos esse dado útil sem fabricar um %.
        try:
            observed_tokens = int(bucket.get("week_tokens", bucket.get("tokens")))
        except (TypeError, ValueError):
            observed_tokens = None
        if observed_tokens is not None and observed_tokens >= 0:
            return {
                "session_pct": 0,
                "week_pct": 0,
                "session_reset_min": None,
                "week_reset_min": None,
                "reset_min": None,
                "week_remaining_pct": None,
                "week_used_pct": None,
                "week_tokens": observed_tokens,
                "quota_kind": "observed_tokens",
                "source": provider,
                "stale": stale,
                "available": True,
                "token_only": True,
                "observed_epoch": observed,
                "snapshot_age_seconds": age,
            }
        return _external_quota_unavailable(provider + ":sem-percentual")

    reset_epoch = _coerce_epoch(bucket.get("reset_time"), now)
    if reset_epoch is None:
        reset_epoch = _coerce_epoch(bucket.get("reset_at"), now)
    if reset_epoch is None:
        reset_epoch = _coerce_epoch(bucket.get("reset_epoch"), now)
    if reset_epoch is None and bucket.get("reset_in_seconds") is not None:
        reset_epoch = _coerce_epoch(bucket.get("reset_in_seconds"), now)
    reset_min = future_minutes_until(
        datetime.fromtimestamp(reset_epoch, tz=timezone.utc), now
    ) if reset_epoch else None
    if reset_epoch and reset_epoch <= int(now.timestamp()):
        stale = True
        reset_epoch = None
        reset_min = None

    remaining_pct = clamp_pct(remaining_fraction * 100)
    used_pct = clamp_pct(used_fraction * 100)
    return {
        "session_pct": 0,
        "week_pct": used_pct,
        "session_reset_min": None,
        "week_reset_min": reset_min,
        "reset_min": reset_min,
        "week_remaining_pct": remaining_pct,
        "week_used_pct": used_pct,
        "week_reset_epoch": reset_epoch,
        "quota_kind": "weekly",
        "source": provider,
        "stale": stale,
        "available": True,
        "observed_epoch": observed,
        "snapshot_age_seconds": age,
    }


def _grok_bot_blob_key(path):
    """Decodifica a chave base32 usada pelo cache do Grok Bot.

    O Codenotch não lê cookies nem o banco do navegador: o Grok Bot mantém
    blobs nomeados por chaves base32 dentro de sand-client-persistence.
    """
    name = os.path.basename(path)
    if not name.endswith(".blob"):
        return None
    encoded = name[:-5].upper()
    encoded += "=" * ((8 - len(encoded) % 8) % 8)
    try:
        return base64.b32decode(encoded).decode("utf-8")
    except Exception:
        return None


def _read_grok_bot_cache(now=None):
    """Lê a leitura semanal oficial persistida pelo Grok Bot.

    A estrutura é a mesma que o Codenotch chama de GrokBotCache: o slot
    ativo aponta para account.<slot>.weekly-usage.cache, cujo JSON contém
    usage.percentUsed, usage.nextResetMs e reading/expiresAtMs.
    """
    if now is None:
        now = now_utc()
    root = os.path.expanduser(GROK_BOT_PERSISTENCE_PATH)
    try:
        paths = glob.glob(os.path.join(root, "*.blob"))
    except (OSError, TypeError):
        return None
    if not paths:
        return None

    decoded = {}
    for path in paths:
        key = _grok_bot_blob_key(path)
        if key:
            decoded[key] = path

    active_slot = None
    slot_path = decoded.get(GROK_BOT_SLOT_KEY)
    if slot_path:
        try:
            slot_payload = _read_json_file(slot_path)
            slot_value = slot_payload.get("value") if isinstance(slot_payload, dict) else None
            if isinstance(slot_value, str) and slot_value:
                active_slot = slot_value
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            pass

    prefix = "sand.client.slice.account."
    suffix = GROK_BOT_WEEKLY_SUFFIX
    candidates = []
    for key, path in decoded.items():
        if not key.startswith(prefix) or not key.endswith(suffix):
            continue
        slot = key[len(prefix):-len(suffix)]
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            mtime = 0
        # O slot ativo tem prioridade; depois usamos o blob mais recente para
        # atravessar trocas de conta sem exibir o valor de outra conta.
        candidates.append((slot == active_slot, mtime, path))
    candidates.sort(key=lambda item: (item[0], item[1]), reverse=True)

    for _is_active, mtime, path in candidates:
        try:
            payload = _read_json_file(path)
        except (OSError, TypeError, ValueError, json.JSONDecodeError):
            continue
        if not isinstance(payload, dict):
            continue
        value = payload.get("value")
        if not isinstance(value, dict) or value.get("kind") != "present":
            continue
        reading = value.get("reading")
        usage = reading.get("usage") if isinstance(reading, dict) else None
        if not isinstance(usage, dict):
            continue
        try:
            raw_percent = float(usage.get("percentUsed"))
        except (TypeError, ValueError):
            continue
        if not math.isfinite(raw_percent):
            continue

        observed_ms = reading.get("readAtMs") if isinstance(reading, dict) else None
        expires_ms = value.get("expiresAtMs")
        reset_ms = usage.get("nextResetMs")
        try:
            observed_epoch = int(float(observed_ms) / 1000) if observed_ms is not None else int(mtime)
        except (TypeError, ValueError):
            observed_epoch = int(mtime)
        try:
            expires_epoch = int(float(expires_ms) / 1000) if expires_ms is not None else None
        except (TypeError, ValueError):
            expires_epoch = None
        try:
            reset_epoch = int(float(reset_ms) / 1000) if reset_ms is not None else None
        except (TypeError, ValueError):
            reset_epoch = None

        stale = expires_epoch is not None and expires_epoch <= int(now.timestamp())
        if reset_epoch is not None and reset_epoch <= int(now.timestamp()):
            stale = True
            reset_epoch = None
        reset_dt = (datetime.fromtimestamp(reset_epoch, tz=timezone.utc)
                    if reset_epoch is not None else None)
        used_pct = clamp_pct(raw_percent)
        plan_label = usage.get("grokPlanLabel") or usage.get("unifiedPlanName")
        return {
            "session_pct": 0,
            "week_pct": used_pct,
            "session_reset_min": None,
            "week_reset_min": future_minutes_until(reset_dt, now),
            "reset_min": future_minutes_until(reset_dt, now),
            "week_remaining_pct": max(0, 100 - used_pct),
            "week_used_pct": used_pct,
            "week_reset_epoch": reset_epoch,
            "quota_kind": "weekly",
            "source": "grokbot-cache",
            "plan_label": plan_label if isinstance(plan_label, str) else "",
            "stale": stale,
            "available": True,
            "token_only": False,
            "observed_epoch": observed_epoch,
        }
    return None


def _grok_credentials():
    """Seleciona somente a sessão do emissor oficial auth.x.ai."""
    try:
        root = _read_json_file(GROK_AUTH_PATH)
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return None
    if not isinstance(root, dict):
        return None
    entries = []
    for key, value in root.items():
        if not isinstance(value, dict):
            continue
        issuer = key.split("::", 1)[0]
        if issuer != "https://auth.x.ai" and value.get("oidc_issuer") != "https://auth.x.ai":
            continue
        token = value.get("key")
        if not isinstance(token, str) or not token:
            continue
        expires = parse_iso_ts(value.get("expires_at"))
        entry = {
            "access_token": token,
            "expires": expires,
            "refresh_token": value.get("refresh_token"),
            "client_id": value.get("oidc_client_id"),
        }
        entries.append(entry)
    if not entries:
        return None
    live = [entry for entry in entries
            if entry["expires"] is None or entry["expires"] > now_utc()]
    return (live or entries)[0]


def _grok_refresh_access_token(credentials):
    """Renova o token em memória, como o Codenotch, sem alterar auth.json."""
    global _grok_renewed_token
    refresh_token = credentials.get("refresh_token")
    client_id = credentials.get("client_id")
    if not isinstance(refresh_token, str) or not refresh_token:
        return None
    if not isinstance(client_id, str) or not client_id:
        return None
    if (_grok_renewed_token and
            _grok_renewed_token.get("from") == refresh_token and
            _grok_renewed_token.get("expires", 0) > time.time()):
        return _grok_renewed_token["token"]
    body = urllib.parse.urlencode({
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": client_id,
    }).encode("utf-8")
    request = urllib.request.Request(
        GROK_TOKEN_URL,
        data=body,
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
            payload = json.load(response)
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return None
    token = payload.get("access_token") if isinstance(payload, dict) else None
    if not isinstance(token, str) or not token:
        return None
    try:
        lifetime = max(300, int(payload.get("expires_in", 3600)))
    except (TypeError, ValueError):
        lifetime = 3600
    _grok_renewed_token = {
        "token": token,
        "from": refresh_token,
        "expires": time.time() + lifetime,
    }
    return token


def _grok_access_token(credentials):
    expires = credentials.get("expires")
    if expires is None or expires > now_utc():
        return credentials.get("access_token")
    return _grok_refresh_access_token(credentials)


def _read_grok_official_usage(now=None):
    """Lê a cota semanal do endpoint oficial que o Grok CLI usa."""
    global _grok_api_cache
    if now is None:
        now = now_utc()
    if (_grok_api_cache[1] is not None and
            time.monotonic() - _grok_api_cache[0] < GROK_REFRESH_INTERVAL_SECONDS):
        return dict(_grok_api_cache[1])
    credentials = _grok_credentials()
    if credentials is None:
        return None
    token = _grok_access_token(credentials)
    if not token:
        return None
    request = urllib.request.Request(
        GROK_CREDITS_URL,
        headers={
            "Authorization": "Bearer " + token,
            "X-XAI-Token-Auth": "xai-grok-cli",
            "Accept": "application/json",
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(request, timeout=API_TIMEOUT_SECONDS) as response:
            payload = json.load(response)
    except (OSError, ValueError, TypeError, json.JSONDecodeError):
        return None
    config = payload.get("config", {}) if isinstance(payload, dict) else {}
    if not isinstance(config, dict):
        return None

    percent = config.get("creditUsagePercent")
    if percent is None:
        products = config.get("productUsage")
        if isinstance(products, list) and products:
            first = products[0]
            if isinstance(first, dict):
                percent = first.get("usagePercent")
    try:
        used_pct = clamp_pct(float(percent))
    except (TypeError, ValueError):
        used_pct = None
    if used_pct is None:
        # Um período semanal sem uso ainda é uma leitura oficial válida.
        period = config.get("currentPeriod")
        if not isinstance(period, dict) or "end" not in period:
            return None
        used_pct = 0

    period = config.get("currentPeriod")
    reset = None
    if isinstance(period, dict):
        reset = _iso_or_epoch_to_datetime(period.get("end"))
    if reset is None:
        reset = _iso_or_epoch_to_datetime(config.get("billingPeriodEnd"))
    result = {
        "session_pct": 0,
        "week_pct": used_pct,
        "session_reset_min": None,
        "week_reset_min": future_minutes_until(reset, now),
        "reset_min": future_minutes_until(reset, now),
        "week_remaining_pct": max(0, 100 - used_pct),
        "week_used_pct": used_pct,
        "week_reset_epoch": int(reset.timestamp()) if reset else None,
        "quota_kind": "weekly",
        "source": "grok-api",
        "stale": False,
        "available": True,
        "token_only": False,
        "observed_epoch": int(now.timestamp()),
    }
    _grok_api_cache = (time.monotonic(), result)
    return dict(result)


def _read_grok_cli_sessions(now=None):
    """Soma tokens das sessões Grok dos últimos sete dias.

    O CLI registra consumo por sessão, mas não publica a cota do plano. Esse
    caminho é deliberadamente token-only: não convertemos tokens em um
    percentual de limite desconhecido.
    """
    if now is None:
        now = now_utc()
    if not os.path.isfile(os.path.expanduser(GROK_BIN)):
        return None
    try:
        listed = subprocess.run(
            [GROK_BIN, "sessions", "list", "--limit", "100"],
            capture_output=True, text=True, timeout=20, check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if listed.returncode != 0:
        return None

    session_ids = []
    cutoff = now - timedelta(days=7)
    session_pattern = re.compile(
        r"^([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\s+"
        r"(\d{4}-\d{2}-\d{2})\s+(\d{2}:\d{2})"
    )
    for line in listed.stdout.splitlines():
        match = session_pattern.match(line.strip())
        if not match:
            continue
        try:
            updated = datetime.strptime(
                match.group(2) + " " + match.group(3), "%Y-%m-%d %H:%M"
            ).replace(tzinfo=now.tzinfo)
        except ValueError:
            continue
        if updated >= cutoff:
            session_ids.append(match.group(1))

    if not session_ids:
        return None

    total_tokens = 0
    latest_epoch = 0
    sessions_count = 0
    for session_id in session_ids:
        try:
            usage = subprocess.run(
                [GROK_BIN, "usage", session_id],
                capture_output=True, text=True, timeout=8, check=False,
            )
            if usage.returncode != 0:
                continue
            payload = json.loads(usage.stdout)
            session = payload.get("session", {})
            tokens = int(session.get("totalTokens", 0) or 0)
            updated_at = parse_iso_ts(payload.get("updatedAt"))
            if updated_at is not None:
                latest_epoch = max(latest_epoch, int(updated_at.timestamp()))
            total_tokens += max(0, tokens)
            sessions_count += 1
        except (OSError, subprocess.SubprocessError, ValueError,
                TypeError, json.JSONDecodeError):
            continue
    if sessions_count == 0:
        return None
    return {
        "week_tokens": total_tokens,
        "observed_sessions": sessions_count,
        "latest_epoch": latest_epoch,
    }


def _schedule_grok_refresh(path=GROK_QUOTA_PATH, force=False):
    """Atualiza um snapshot local de tokens do Grok sem bloquear /quota."""
    global _grok_refresh_started, _grok_refresh_running
    now_mono = time.monotonic()
    with _grok_refresh_lock:
        if _grok_refresh_running:
            return
        if (not force and
                now_mono - _grok_refresh_started < GROK_REFRESH_INTERVAL_SECONDS):
            return
        _grok_refresh_started = now_mono
        _grok_refresh_running = True

    def worker():
        global _grok_refresh_running
        try:
            value = _read_grok_cli_sessions()
            if value is not None:
                _write_json_file(path, {
                    "source": "grok-cli",
                    "updated_epoch": int(time.time()),
                    "weekly": {
                        "week_tokens": value["week_tokens"],
                        "observed_sessions": value["observed_sessions"],
                    },
                })
        finally:
            with _grok_refresh_lock:
                _grok_refresh_running = False

    threading.Thread(target=worker, daemon=True).start()


def _schedule_antigravity_refresh(path=ANTIGRAVITY_QUOTA_PATH, force=False):
    """Atualiza a cota via /usage sem bloquear o endpoint do ESP32.

    O comando oficial do Antigravity retorna buckets estruturados, inclusive
    quando não há uma sessão interativa com statusLine aberta. O snapshot fica
    limitado a percentuais e timestamps; nenhum texto da conversa é salvo.
    """
    global _antigravity_refresh_started, _antigravity_refresh_running
    now_mono = time.monotonic()
    with _antigravity_refresh_lock:
        if _antigravity_refresh_running:
            return
        if (not force and
                now_mono - _antigravity_refresh_started < ANTIGRAVITY_REFRESH_INTERVAL_SECONDS):
            return
        _antigravity_refresh_started = now_mono
        _antigravity_refresh_running = True

    def worker():
        global _antigravity_refresh_running
        try:
            completed = subprocess.run(
                [ANTIGRAVITY_BIN, "--print-timeout", "30s",
                 "--output-format", "json", "--print", "/usage"],
                capture_output=True,
                text=True,
                timeout=35,
                check=False,
            )
            if completed.returncode != 0:
                return
            result = json.loads(completed.stdout)
            data = result.get("command", {}).get("data", {})
            groups = data.get("groups", [])
            buckets = {}
            for group in groups:
                for bucket in group.get("buckets", []):
                    bucket_id = bucket.get("id")
                    remaining = bucket.get("remaining_fraction")
                    if not isinstance(bucket_id, str) or remaining is None:
                        continue
                    safe = {"remaining_fraction": remaining}
                    for field in ("reset_time", "reset_in_seconds"):
                        if field in bucket:
                            safe[field] = bucket[field]
                    buckets[bucket_id] = safe
            if not buckets:
                return
            _write_json_file(path, {
                "source": "antigravity-usage",
                "updated_epoch": int(time.time()),
                "quota": buckets,
            })
        except (OSError, subprocess.SubprocessError, ValueError, TypeError,
                json.JSONDecodeError):
            return
        finally:
            with _antigravity_refresh_lock:
                _antigravity_refresh_running = False

    threading.Thread(target=worker, daemon=True).start()


def collect_antigravity_usage(now=None, path=ANTIGRAVITY_QUOTA_PATH):
    """Lê Gemini semanal e agenda /usage quando o snapshot não está atual."""
    value = _read_external_weekly_quota(
        path, "antigravity-usage", bucket_id="gemini-weekly", now=now
    )
    if not value.get("available") or value.get("stale"):
        _schedule_antigravity_refresh(path)
    return value


def collect_grok_usage(now=None, path=GROK_QUOTA_PATH):
    """Lê primeiro o Grok Bot Plan; mantém CLI/API como fallback.

    O cache do aplicativo é a fonte correta para o plano mostrado no Grok
    Bot. O endpoint de créditos pertence ao Grok CLI e só deve ser usado
    quando o aplicativo não deixou uma leitura local disponível.
    """
    bot_cache = _read_grok_bot_cache(now=now)
    if bot_cache is not None:
        return bot_cache
    official = _read_grok_official_usage(now=now)
    if official is not None:
        return official
    value = _read_external_weekly_quota(path, "grok-cli", now=now)
    if not value.get("available") or value.get("stale"):
        _schedule_grok_refresh(path)
    return value


class ClaudeApiRateLimited(Exception):
    """A API informou quanto tempo devemos esperar antes de consultar novamente."""


def _retry_after_seconds(headers):
    try:
        return max(60, int(headers.get("retry-after", "60")))
    except (AttributeError, TypeError, ValueError):
        return 60


def _claude_credentials():
    """Lê OAuth do arquivo legado ou do Keychain usado pelo Claude Code no macOS."""
    try:
        credentials = _read_json_file("~/.claude/.credentials.json")
        # Versões atuais do Claude Code mantêm neste arquivo apenas OAuth de
        # MCP. Só é uma credencial Claude válida quando o bloco first-party
        # claudeAiOauth está presente; caso contrário, consulte o Keychain.
        if isinstance(credentials, dict) and isinstance(
                credentials.get("claudeAiOauth"), dict):
            return credentials
    except (OSError, json.JSONDecodeError):
        pass
    raw = subprocess.check_output(
        ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
        text=True, timeout=5, stderr=subprocess.DEVNULL,
    )
    return json.loads(raw)


_claude_api_next_retry_monotonic = 0.0


def save_claude_api_snapshot(value, path=CLAUDE_API_SNAPSHOT_PATH):
    """Persiste somente a última leitura oficial útil para atravessar reinícios."""
    snapshot = {
        "session_pct": clamp_pct(value.get("session_pct")),
        "week_pct": clamp_pct(value.get("week_pct")),
        "session_reset_epoch": value.get("session_reset_epoch"),
        "week_reset_epoch": value.get("week_reset_epoch"),
        "observed_epoch": value.get("observed_epoch"),
    }
    _write_json_file(path, snapshot)


def load_claude_api_snapshot(now=None, path=CLAUDE_API_SNAPSHOT_PATH):
    """Lê um reset oficial anterior apenas enquanto ele ainda estiver no futuro."""
    if now is None:
        now = now_utc()
    try:
        snapshot = _read_json_file(path)
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return None
    result = {}
    for window in ("session", "week"):
        epoch = snapshot.get(window + "_reset_epoch")
        reset = _iso_or_epoch_to_datetime(epoch)
        if reset is not None and reset > now:
            result[window + "_reset_epoch"] = int(reset.timestamp())
            result[window + "_reset_min"] = minutes_until(reset, now)
    return result or None


def merge_claude_reset_snapshot(value, snapshot):
    """Completa somente resets ausentes; uma leitura atual sempre tem prioridade."""
    result = dict(value)
    if not snapshot:
        return result
    applied = False
    for window in ("session", "week"):
        reset_key = window + "_reset_min"
        epoch_key = window + "_reset_epoch"
        if result.get(reset_key) is None and snapshot.get(reset_key) is not None:
            result[reset_key] = snapshot[reset_key]
            result[epoch_key] = snapshot[epoch_key]
            applied = True
    if applied:
        result["reset_snapshot"] = True
        result["source"] = result.get("source", "logs") + "+snapshot"
    return result


def fetch_claude_api_usage(now=None):
    global _claude_api_cache, _claude_api_next_retry_monotonic
    if now is None:
        now = now_utc()
    if _claude_api_cache[1] is not None and time.monotonic() - _claude_api_cache[0] < CLAUDE_API_CACHE_SECONDS:
        return dict(_claude_api_cache[1])
    if time.monotonic() < _claude_api_next_retry_monotonic:
        raise ClaudeApiRateLimited()
    credentials = _claude_credentials()
    token = credentials["claudeAiOauth"]["accessToken"]
    try:
        payload = _http_json(
            "https://api.anthropic.com/api/oauth/usage",
            {
                "Authorization": "Bearer " + token,
                "anthropic-beta": "oauth-2025-04-20",
                "User-Agent": "claude-code/2.1.90",
            },
        )
    except urllib.error.HTTPError as error:
        if error.code == 429:
            _claude_api_next_retry_monotonic = time.monotonic() + _retry_after_seconds(error.headers)
            raise ClaudeApiRateLimited() from error
        raise
    session = payload.get("five_hour") or {}
    week = payload.get("seven_day") or {}
    session_reset = parse_iso_ts(session.get("resets_at")) if session.get("resets_at") else None
    week_reset = parse_iso_ts(week.get("resets_at")) if week.get("resets_at") else None
    result = {
        "session_pct": clamp_pct(session.get("utilization")),
        "session_reset_epoch": int(session_reset.timestamp()) if session_reset else None,
        "week_reset_epoch": int(week_reset.timestamp()) if week_reset else None,
        "observed_epoch": int(now.timestamp()),
        "week_pct": clamp_pct(week.get("utilization")),
        "session_reset_min": minutes_until(session_reset, now),
        "week_reset_min": minutes_until(week_reset, now),
        "source": "api",
    }
    _claude_api_cache = (time.monotonic(), result)
    save_claude_api_snapshot(result)
    return dict(result)


def load_claude_usage_cache(now=None, path="~/.claude/usage-cache.json"):
    """Usa o cache oficial atualizado pelo Claude Code quando a API responde 429."""
    if now is None:
        now = now_utc()
    try:
        payload = _read_json_file(path)
        fetched = parse_iso_ts(payload.get("fetched_at"))
        if fetched is None:
            return None
        age = max(0, int((now - fetched).total_seconds()))
        # O endpoint Anthropic pode aplicar rate-limit por várias horas.
        # Preserve o último valor conhecido para não exibir zero falso.
        if age > 7 * 86400:
            return None
        session_reset = parse_iso_ts(payload.get("five_hour_resets_at"))
        # O timestamp do cache é a fronteira real da janela de 5 h. Depois
        # que ela expira, não é válido repetir o percentual antigo em uma
        # janela inventada: o Claude Code volta a reportar 0% até chegar uma
        # leitura oficial nova.
        session_expired = session_reset is not None and session_reset <= now
        if session_expired:
            session_reset = None
        return {
            "session_pct": 0 if session_expired else clamp_pct(payload.get("five_hour")),
            "week_pct": clamp_pct(payload.get("seven_day")),
            "session_reset_min": future_minutes_until(session_reset, now),
            "week_reset_min": future_minutes_until(
                parse_iso_ts(payload.get("seven_day_resets_at")), now),
            "source": "cache",
            "stale": bool(payload.get("stale")) or age > 120,
            "reset_estimated": False,
        }
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        return None


def merge_claude_fallback(local, cached):
    """Combina fontes preservando o percentual oficial do cache.

    Os JSONL locais registram tokens processados, mas não conhecem os limites
    reais da conta Claude. Portanto, eles servem para tokens e projeção; não
    podem substituir os percentuais oficiais por uma razão arbitrária de
    tokens/limite configurável.
    """
    if not cached:
        return dict(local)
    result = {**local, **cached}
    for window in ("session", "week"):
        pct_key = f"{window}_pct"
        reset_key = f"{window}_reset_min"
        result[pct_key] = clamp_pct(cached.get(pct_key))
        if cached.get(reset_key) is None and local.get(reset_key) is not None:
            result[reset_key] = local[reset_key]
    result["source"] = "cache+logs"
    result["stale"] = bool(cached.get("stale"))
    return result


def fetch_codex_api_usage(now=None):
    global _codex_api_cache
    if now is None:
        now = now_utc()
    if _codex_api_cache[1] is not None and time.monotonic() - _codex_api_cache[0] < CODEX_API_CACHE_SECONDS:
        return dict(_codex_api_cache[1])
    credentials = _read_json_file("~/.codex/auth.json")
    token_data = credentials["tokens"]
    headers = {
        "Authorization": "Bearer " + token_data["access_token"],
        "User-Agent": "token-quota-display/2.0",
    }
    account_id = token_data.get("account_id")
    if account_id:
        headers["chatgpt-account-id"] = account_id
    payload = _http_json("https://chatgpt.com/backend-api/wham/usage", headers)
    rate_limit = payload.get("rate_limit") or {}
    session = rate_limit.get("primary_window") or {}
    week = rate_limit.get("secondary_window") or {}
    result = {
        "session_pct": clamp_pct(session.get("used_percent")),
        "session_reset_epoch": session.get("reset_at"),
        "week_reset_epoch": week.get("reset_at"),
        "observed_epoch": int(now.timestamp()),
        "week_pct": clamp_pct(week.get("used_percent")),
        "session_reset_min": minutes_until(session.get("reset_at"), now),
        "week_reset_min": minutes_until(week.get("reset_at"), now),
        "source": "api",
    }
    _codex_api_cache = (time.monotonic(), result)
    return dict(result)


_claude_api_cache = (0.0, None)
_codex_api_cache = (0.0, None)


# --------------------------------------------------------------------------
# Limites calibráveis
# --------------------------------------------------------------------------

def load_limits(path=LIMITS_PATH):
    """Carrega limits.json. Tolerante a arquivo ausente ou corrompido: cai no default."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        claude_cfg = data.get("claude", {})
        merged = dict(_DEFAULT_LIMITS["claude"])
        merged.update({k: v for k, v in claude_cfg.items() if k in merged})
        return {"claude": merged}
    except (OSError, json.JSONDecodeError, AttributeError):
        return dict(_DEFAULT_LIMITS)


# --------------------------------------------------------------------------
# Coleta Claude Code
# --------------------------------------------------------------------------

def _iter_jsonl_lines(path):
    """Gera dicts válidos de um arquivo JSONL, ignorando linhas corrompidas."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue
    except OSError:
        return


def collect_claude_usage(now=None, projects_glob=CLAUDE_PROJECTS_GLOB, limits=None):
    """
    Varre os JSONL de projetos do Claude Code e soma tokens por janela rolante.
    Não há percentual de cota explícito na fonte (ver VALIDATION.md), então o
    percentual é derivado de soma de tokens contra um limite calibrável.
    """
    if now is None:
        now = now_utc()
    if limits is None:
        limits = load_limits()["claude"]

    session_start = now - timedelta(hours=limits["session_window_hours"])
    week_start = now - timedelta(days=limits["week_window_days"])

    session_tokens = 0
    week_tokens = 0
    oldest_ts_in_session = None
    oldest_ts_in_week = None

    for path in glob.glob(projects_glob):
        for entry in _iter_jsonl_lines(path):
            ts = parse_iso_ts(entry.get("timestamp"))
            if ts is None:
                continue
            message = entry.get("message")
            if not isinstance(message, dict):
                continue
            usage = message.get("usage")
            if not isinstance(usage, dict):
                continue

            tokens = (
                (usage.get("input_tokens") or 0)
                + (usage.get("cache_creation_input_tokens") or 0)
                + (usage.get("cache_read_input_tokens") or 0)
                + (usage.get("output_tokens") or 0)
            )

            if ts >= week_start:
                week_tokens += tokens
                if oldest_ts_in_week is None or ts < oldest_ts_in_week:
                    oldest_ts_in_week = ts
            if ts >= session_start:
                session_tokens += tokens
                if oldest_ts_in_session is None or ts < oldest_ts_in_session:
                    oldest_ts_in_session = ts

    session_pct = clamp_pct(session_tokens * 100 / limits["session_token_limit"])
    week_pct = clamp_pct(week_tokens * 100 / limits["week_token_limit"])

    session_reset_at = None
    if oldest_ts_in_session is not None:
        session_reset_at = oldest_ts_in_session + timedelta(hours=limits["session_window_hours"])
    week_reset_at = None
    if oldest_ts_in_week is not None:
        week_reset_at = oldest_ts_in_week + timedelta(days=limits["week_window_days"])

    session_reset_min = minutes_until(session_reset_at, now)
    week_reset_min = minutes_until(week_reset_at, now)
    return {
        "session_pct": session_pct,
        "week_pct": week_pct,
        "session_tokens": session_tokens,
        "week_tokens": week_tokens,
        "session_reset_min": session_reset_min,
        "week_reset_min": week_reset_min,
        # Compatibilidade com o firmware anterior: reset da janela curta.
        "reset_min": session_reset_min,
    }


# --------------------------------------------------------------------------
# Coleta Codex
# --------------------------------------------------------------------------

def _latest_codex_session_file(sessions_glob=CODEX_SESSIONS_GLOB):
    files = glob.glob(sessions_glob, recursive=True)
    if not files:
        return None
    return max(files, key=lambda p: os.path.getmtime(p))


def collect_codex_usage(now=None, sessions_glob=CODEX_SESSIONS_GLOB):
    """
    Lê o arquivo de sessão mais recente do Codex e extrai o último evento
    rate_limits válido. primary = janela de 5h (300 min), secondary = janela
    de 7 dias (10080 min) — mapeamento por window_minutes, não por posição,
    pois sessões antigas só têm uma janela (ver VALIDATION.md secao 3).
    Se rate_limits nunca aparecer no arquivo, os campos ficam em ABERTO (None).
    """
    if now is None:
        now = now_utc()

    latest_file = _latest_codex_session_file(sessions_glob)
    session_pct = None
    week_pct = None
    session_resets_at = None
    week_resets_at = None

    if latest_file is not None:
        for entry in _iter_jsonl_lines(latest_file):
            payload = entry.get("payload")
            if not isinstance(payload, dict):
                continue
            rate_limits = payload.get("rate_limits")
            if not isinstance(rate_limits, dict):
                continue

            for window in ("primary", "secondary"):
                w = rate_limits.get(window)
                if not isinstance(w, dict):
                    continue
                minutes = w.get("window_minutes")
                used_percent = w.get("used_percent")
                resets_at = w.get("resets_at")
                if minutes == 300:
                    session_pct = used_percent
                    session_resets_at = resets_at
                elif minutes == 10080:
                    week_pct = used_percent
                    week_resets_at = resets_at

    # Um evento antigo não pode sobreviver ao próprio reset. Isso evita o
    # estado enganoso "54% / renova 0m" quando o Codex fica ocioso.
    if session_resets_at is not None and not _window_is_live(session_resets_at, now):
        session_pct = 0
        session_resets_at = None
    if week_resets_at is not None and not _window_is_live(week_resets_at, now):
        week_pct = 0
        week_resets_at = None

    session_reset_min = minutes_until(session_resets_at, now)
    week_reset_min = minutes_until(week_resets_at, now)
    reset_min = session_reset_min if session_reset_min is not None else week_reset_min

    return {
        "session_pct": clamp_pct(session_pct) if session_pct is not None else 0,
        "week_pct": clamp_pct(week_pct) if week_pct is not None else 0,
        "session_tokens": 0,
        "week_tokens": 0,
        "session_reset_min": session_reset_min,
        "week_reset_min": week_reset_min,
        "reset_min": reset_min,
    }


def collect_codex_tokens(now=None, sessions_glob=CODEX_SESSIONS_GLOB):
    """Soma o custo de cada resposta registrado em last_token_usage."""
    if now is None:
        now = now_utc()
    session_start = now - timedelta(hours=5)
    week_start = now - timedelta(days=7)
    session_tokens = 0
    week_tokens = 0
    for path in glob.glob(sessions_glob, recursive=True):
        for entry in _iter_jsonl_lines(path):
            ts = parse_iso_ts(entry.get("timestamp"))
            if ts is None or ts < week_start:
                continue
            payload = entry.get("payload")
            info = payload.get("info") if isinstance(payload, dict) else None
            last = info.get("last_token_usage") if isinstance(info, dict) else None
            if not isinstance(last, dict):
                continue
            try:
                tokens = max(0, int(last.get("total_tokens") or 0))
            except (TypeError, ValueError):
                continue
            week_tokens += tokens
            if ts >= session_start:
                session_tokens += tokens
    return session_tokens, week_tokens


# --------------------------------------------------------------------------
# Histórico e projeção
# --------------------------------------------------------------------------

def load_history(path=HISTORY_PATH):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            values = json.load(fh)
        return values if isinstance(values, list) else []
    except (OSError, json.JSONDecodeError):
        return []


def save_history(values, path=HISTORY_PATH):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(values, fh, separators=(",", ":"))
        os.replace(tmp, path)
    except OSError:
        pass


def calculate_projection(samples, model, now_epoch, pct, reset_min):
    """Projeta uso no reset usando somente crescimento positivo dos últimos 60 min."""
    recent = [s for s in samples
              if now_epoch - 3600 <= s.get("ts", 0) <= now_epoch
              and isinstance(s.get(model), (int, float))]
    # A queda de utilização marca nova janela ou correção da fonte.
    for i in range(len(recent) - 1, 0, -1):
        if recent[i][model] < recent[i - 1][model]:
            recent = recent[i:]
            break
    if len(recent) < 3 or reset_min is None or reset_min <= 0:
        return {"projection_min": None, "projected_pct_at_reset": None}
    first, last = recent[0], recent[-1]
    elapsed_min = (last["ts"] - first["ts"]) / 60.0
    delta = last[model] - first[model]
    # Dez minutos/3 amostras evitam previsões dramáticas por um único salto.
    if elapsed_min < 10:
        return {"projection_min": None, "projected_pct_at_reset": None}
    if delta <= 0:
        return {"projection_min": None, "projected_pct_at_reset": pct}
    rate = delta / elapsed_min
    exhaustion = int(round((100 - pct) / rate)) if pct < 100 else 0
    projected = clamp_pct(pct + rate * reset_min)
    return {
        "projection_min": exhaustion if 0 <= exhaustion <= reset_min else None,
        "projected_pct_at_reset": projected,
    }


def _record_history(history, now_epoch, claude_pct, codex_pct):
    if not history or now_epoch - history[-1].get("ts", 0) >= CACHE_SECONDS:
        history.append({"ts": now_epoch, "claude": claude_pct, "codex": codex_pct})
    cutoff = now_epoch - 8 * 24 * 3600
    return [sample for sample in history if sample.get("ts", 0) >= cutoff][-2500:]


# --------------------------------------------------------------------------
# Agregação + cache
# --------------------------------------------------------------------------

def build_quota_payload(now=None):
    if now is None:
        now = now_utc()
    limits = load_limits()
    claude_local = collect_claude_usage(now=now, limits=limits["claude"])
    codex_local = collect_codex_usage(now=now)
    codex_session_tokens, codex_week_tokens = collect_codex_tokens(now=now)
    codex_local["session_tokens"] = codex_session_tokens
    codex_local["week_tokens"] = codex_week_tokens
    claude_local["source"] = "logs"
    codex_local["source"] = "logs"

    try:
        claude = {**claude_local, **fetch_claude_api_usage(now=now)}
    except (OSError, KeyError, ValueError, json.JSONDecodeError,
            subprocess.SubprocessError, urllib.error.URLError, ClaudeApiRateLimited):
        claude = merge_claude_fallback(
            claude_local, load_claude_usage_cache(now=now))
        # O cache do Claude Code pode conter percentual mas omitir o reset
        # de 5h. O snapshot preserva a última resposta oficial até o fim
        # daquela janela, inclusive após reiniciar o Mac ou o LaunchAgent.
        claude = merge_claude_reset_snapshot(
            claude, load_claude_api_snapshot(now=now))
    if claude.get("source") in ("api", "cache", "cache+logs") and not claude.get("session_tokens"):
        claude["session_tokens"] = int(
            limits["claude"]["session_token_limit"] * claude["session_pct"] / 100)
        claude["tokens_estimated"] = True
    else:
        claude["tokens_estimated"] = False
    try:
        codex = {**codex_local, **fetch_codex_api_usage(now=now)}
    except (OSError, KeyError, ValueError, json.JSONDecodeError, urllib.error.URLError):
        codex = codex_local

    # Fontes semanais externas são independentes do cálculo de tokens do
    # Claude/Codex. A ausência do snapshot não vira zero silencioso: o
    # contrato marca a fonte como aguardando/stale para a UI poder explicar.
    gemini = collect_antigravity_usage(now=now)
    grok = collect_grok_usage(now=now)

    claude["reset_min"] = claude.get("session_reset_min")
    codex["reset_min"] = codex.get("session_reset_min")
    now_epoch = int(now.timestamp())
    for value in (claude, codex):
        for window in ("session", "week"):
            key = window + "_reset_epoch"
            if key not in value:
                minutes = value.get(window + "_reset_min")
                value[key] = now_epoch + minutes * 60 if minutes is not None else None
            value[window + "_reset_min"] = minutes_until(value[key], now)
    history = _record_history(
        load_history(), now_epoch, claude["session_pct"], codex["session_pct"])
    save_history(history)
    claude.update(calculate_projection(
        history, "claude", now_epoch, claude["session_pct"], claude.get("session_reset_min")))
    codex.update(calculate_projection(
        history, "codex", now_epoch, codex["session_pct"], codex.get("session_reset_min")))
    for name, value in (("claude", claude), ("codex", codex)):
        reset = value.get("session_reset_epoch")
        start = reset - 18000 if reset else now_epoch - 18000
        # 24 posições fixas na janela; lacunas permanecem desconhecidas.
        points = [-1] * 24
        for sample in history:
            ts = sample.get("ts", 0)
            if start <= ts <= min(now_epoch, start + 17999):
                points[min(23, int((ts - start) / 750))] = clamp_pct(sample[name])
        value["trend"] = points
        for window in ("session", "week"):
            epoch = value.get(window + "_reset_epoch")
            value[window + "_reset_clock"] = (
                datetime.fromtimestamp(epoch).strftime("%d/%m %H:%M") if epoch else "--")
    return {
        "claude": claude,
        "codex": codex,
        "gemini": gemini,
        "grok": grok,
        "updated": now_epoch,
    }


class _QuotaCache:
    """Cache curto, protegido por lock para uso com ThreadingHTTPServer."""

    def __init__(self, ttl_seconds=CACHE_SECONDS):
        self._ttl = ttl_seconds
        self._lock = threading.Lock()
        self._payload = None
        self._fetched_at = 0.0
        self._refreshing = False

    def _refresh(self):
        try:
            payload = build_quota_payload()
            with self._lock:
                self._payload = payload
                self._fetched_at = time.monotonic()
        finally:
            with self._lock:
                self._refreshing = False

    def invalidate(self):
        with self._lock:
            self._fetched_at = 0.0

    def get(self):
        with self._lock:
            now = time.monotonic()
            if self._payload is None:
                # Primeira leitura ainda é síncrona; as seguintes nunca bloqueiam HTTP.
                self._payload = build_quota_payload()
                self._fetched_at = time.monotonic()
            elif (now - self._fetched_at) >= self._ttl and not self._refreshing:
                self._refreshing = True
                threading.Thread(target=self._refresh, daemon=True).start()
            return self._payload


_cache = _QuotaCache()


# --------------------------------------------------------------------------
# Servidor HTTP
# --------------------------------------------------------------------------

class QuotaHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # silencioso; evita poluir stdout do LaunchAgent

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path == "/firmware/manifest.json":
            return self._send_file(FIRMWARE_MANIFEST_PATH, "application/json")
        if parsed.path == "/firmware/token_quota_display.bin":
            return self._send_file(FIRMWARE_BINARY_PATH, "application/octet-stream")
        if parsed.path != "/quota":
            self.send_response(404)
            self.end_headers()
            return

        try:
            force_refresh = urllib.parse.parse_qs(parsed.query).get("refresh") == ["1"]
            if force_refresh:
                _cache.invalidate()
                _schedule_antigravity_refresh(force=True)
            payload = dict(_cache.get())
            payload["server_epoch"] = int(time.time())
            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self.send_response(500)
            self.end_headers()

    def _send_file(self, path, content_type):
        """Serve somente os dois artefatos OTA publicados pelo coletor."""
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as fh:
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(size))
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                while True:
                    chunk = fh.read(8192)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except (OSError, BrokenPipeError):
            self.send_response(404)
            self.end_headers()


def main():
    server = ThreadingHTTPServer((HOST, PORT), QuotaHandler)
    print(f"quota_server escutando em http://{HOST}:{PORT}/quota")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
