#!/usr/bin/env python3
"""
Testes do coletor com fixtures JSONL sintéticas (nenhum dado real é lido).
Roda com: python3 -m unittest test_quota.py -v
"""

import json
import os
import shutil
import tempfile
import unittest
from datetime import datetime, timedelta, timezone

import quota_server as qs


def _write_jsonl(path, entries):
    with open(path, "w", encoding="utf-8") as fh:
        for e in entries:
            fh.write(json.dumps(e) + "\n")


def _claude_line(ts, input_tokens=100, cache_creation=0, cache_read=0, output_tokens=50):
    return {
        "timestamp": ts,
        "type": "assistant",
        "message": {
            "usage": {
                "input_tokens": input_tokens,
                "cache_creation_input_tokens": cache_creation,
                "cache_read_input_tokens": cache_read,
                "output_tokens": output_tokens,
            }
        },
    }


def _codex_rate_limit_line(ts, primary_pct=None, primary_resets_at=None,
                            secondary_pct=None, secondary_resets_at=None):
    rate_limits = {}
    if primary_pct is not None:
        rate_limits["primary"] = {
            "used_percent": primary_pct,
            "window_minutes": 300,
            "resets_at": primary_resets_at,
        }
    if secondary_pct is not None:
        rate_limits["secondary"] = {
            "used_percent": secondary_pct,
            "window_minutes": 10080,
            "resets_at": secondary_resets_at,
        }
    return {
        "timestamp": ts,
        "type": "event_msg",
        "payload": {"type": "token_count", "rate_limits": rate_limits},
    }


class ClaudeUsageTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.project_dir = os.path.join(self.tmpdir, "-Users-fake-projeto")
        os.makedirs(self.project_dir)
        self.glob_pattern = os.path.join(self.tmpdir, "*", "*.jsonl")
        self.limits = {
            "session_window_hours": 5,
            "session_token_limit": 1000,
            "week_window_days": 7,
            "week_token_limit": 10000,
        }

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _write(self, name, entries):
        _write_jsonl(os.path.join(self.project_dir, name), entries)

    def test_janela_5h_soma_apenas_linhas_recentes(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        dentro = (now - timedelta(hours=1)).isoformat().replace("+00:00", "Z")
        fora = (now - timedelta(hours=6)).isoformat().replace("+00:00", "Z")
        self._write("s1.jsonl", [
            _claude_line(dentro, input_tokens=100, output_tokens=50),  # 150 tokens, dentro
            _claude_line(fora, input_tokens=1000, output_tokens=500),  # fora da janela de 5h
        ])
        result = qs.collect_claude_usage(now=now, projects_glob=self.glob_pattern, limits=self.limits)
        # 150 tokens / 1000 limite = 15%
        self.assertEqual(result["session_pct"], 15)
        self.assertEqual(result["session_reset_min"], 240)
        self.assertEqual(result["reset_min"], result["session_reset_min"])

    def test_janela_semanal_inclui_linha_fora_da_janela_de_sessao(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        seis_horas_atras = (now - timedelta(hours=6)).isoformat().replace("+00:00", "Z")
        self._write("s1.jsonl", [
            _claude_line(seis_horas_atras, input_tokens=1000, output_tokens=0),
        ])
        result = qs.collect_claude_usage(now=now, projects_glob=self.glob_pattern, limits=self.limits)
        self.assertEqual(result["session_pct"], 0)  # fora da janela de 5h
        self.assertEqual(result["week_pct"], 10)    # 1000/10000 = 10%, dentro da janela de 7d
        self.assertIsNone(result["session_reset_min"])
        self.assertEqual(result["week_reset_min"], 9720)

    def test_arquivo_vazio(self):
        self._write("vazio.jsonl", [])
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        result = qs.collect_claude_usage(now=now, projects_glob=self.glob_pattern, limits=self.limits)
        self.assertEqual(result, {
            "session_pct": 0, "week_pct": 0,
            "session_tokens": 0, "week_tokens": 0,
            "session_reset_min": None, "week_reset_min": None, "reset_min": None,
        })

    def test_linha_corrompida_e_ignorada_sem_quebrar(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        dentro = (now - timedelta(hours=1)).isoformat().replace("+00:00", "Z")
        path = os.path.join(self.project_dir, "corrompido.jsonl")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(_claude_line(dentro, input_tokens=200, output_tokens=0)) + "\n")
            fh.write("{isso nao é json valido\n")
            fh.write("\n")  # linha em branco
            fh.write(json.dumps(_claude_line(dentro, input_tokens=100, output_tokens=0)) + "\n")
        result = qs.collect_claude_usage(now=now, projects_glob=self.glob_pattern, limits=self.limits)
        self.assertEqual(result["session_pct"], 30)  # (200+100)/1000

    def test_nenhum_arquivo_encontrado(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        glob_vazio = os.path.join(self.tmpdir, "inexistente", "*.jsonl")
        result = qs.collect_claude_usage(now=now, projects_glob=glob_vazio, limits=self.limits)
        self.assertEqual(result, {
            "session_pct": 0, "week_pct": 0,
            "session_tokens": 0, "week_tokens": 0,
            "session_reset_min": None, "week_reset_min": None, "reset_min": None,
        })


class CodexUsageTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.sessions_dir = os.path.join(self.tmpdir, "2026", "08", "28")
        os.makedirs(self.sessions_dir)
        self.glob_pattern = os.path.join(self.tmpdir, "**", "*.jsonl")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _write(self, name, entries):
        _write_jsonl(os.path.join(self.sessions_dir, name), entries)

    def test_rate_limits_primary_e_secondary_mapeados_por_window_minutes(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        resets_primary = int((now + timedelta(hours=2)).timestamp())
        resets_secondary = int((now + timedelta(days=3)).timestamp())
        ts = now.isoformat().replace("+00:00", "Z")
        self._write("rollout-1.jsonl", [
            _codex_rate_limit_line(ts, primary_pct=65.0, primary_resets_at=resets_primary,
                                    secondary_pct=26.0, secondary_resets_at=resets_secondary),
        ])
        result = qs.collect_codex_usage(now=now, sessions_glob=self.glob_pattern)
        self.assertEqual(result["session_pct"], 65)
        self.assertEqual(result["week_pct"], 26)
        self.assertEqual(result["session_reset_min"], 120)
        self.assertEqual(result["week_reset_min"], 4320)
        self.assertEqual(result["reset_min"], result["session_reset_min"])

    def test_usa_ultimo_evento_rate_limits_do_arquivo_mais_recente(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        ts1 = (now - timedelta(minutes=10)).isoformat().replace("+00:00", "Z")
        ts2 = now.isoformat().replace("+00:00", "Z")
        self._write("rollout-1.jsonl", [
            _codex_rate_limit_line(ts1, primary_pct=10.0, primary_resets_at=int(now.timestamp()) + 600),
            _codex_rate_limit_line(ts2, primary_pct=42.0, primary_resets_at=int(now.timestamp()) + 600),
        ])
        result = qs.collect_codex_usage(now=now, sessions_glob=self.glob_pattern)
        self.assertEqual(result["session_pct"], 42)

    def test_ausencia_de_rate_limits_retorna_zeros_e_reset_none(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        ts = now.isoformat().replace("+00:00", "Z")
        self._write("rollout-1.jsonl", [
            {"timestamp": ts, "type": "event_msg", "payload": {"type": "token_count", "info": {}}},
        ])
        result = qs.collect_codex_usage(now=now, sessions_glob=self.glob_pattern)
        self.assertEqual(result, {
            "session_pct": 0, "week_pct": 0,
            "session_tokens": 0, "week_tokens": 0,
            "session_reset_min": None, "week_reset_min": None, "reset_min": None,
        })

    def test_nenhum_arquivo_de_sessao(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        glob_vazio = os.path.join(self.tmpdir, "inexistente", "**", "*.jsonl")
        result = qs.collect_codex_usage(now=now, sessions_glob=glob_vazio)
        self.assertEqual(result, {
            "session_pct": 0, "week_pct": 0,
            "session_tokens": 0, "week_tokens": 0,
            "session_reset_min": None, "week_reset_min": None, "reset_min": None,
        })

    def test_apenas_janela_semanal_sem_primary_como_em_sessoes_antigas(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        ts = now.isoformat().replace("+00:00", "Z")
        resets_secondary = int((now + timedelta(days=1)).timestamp())
        self._write("rollout-1.jsonl", [
            _codex_rate_limit_line(ts, secondary_pct=18.0, secondary_resets_at=resets_secondary),
        ])
        result = qs.collect_codex_usage(now=now, sessions_glob=self.glob_pattern)
        self.assertEqual(result["session_pct"], 0)
        self.assertEqual(result["week_pct"], 18)
        self.assertIsNone(result["session_reset_min"])
        self.assertEqual(result["week_reset_min"], 1440)
        self.assertEqual(result["reset_min"], result["week_reset_min"])

    def test_evento_expirado_nao_fica_preso_em_zero_minutos(self):
        now = datetime(2026, 8, 28, 12, 0, 0, tzinfo=timezone.utc)
        ts = (now - timedelta(hours=6)).isoformat().replace("+00:00", "Z")
        self._write("rollout-1.jsonl", [
            _codex_rate_limit_line(
                ts, primary_pct=54,
                primary_resets_at=int((now - timedelta(hours=1)).timestamp())),
        ])
        result = qs.collect_codex_usage(now=now, sessions_glob=self.glob_pattern)
        self.assertEqual(result["session_pct"], 0)
        self.assertIsNone(result["session_reset_min"])


class ProjectionTests(unittest.TestCase):
    def test_projeta_esgotamento_antes_do_reset(self):
        now = 10_000
        samples = [
            {"ts": now - 1800, "claude": 20},
            {"ts": now - 900, "claude": 35},
            {"ts": now, "claude": 50},
        ]
        result = qs.calculate_projection(samples, "claude", now, 50, 120)
        self.assertEqual(result["projection_min"], 50)
        self.assertEqual(result["projected_pct_at_reset"], 100)

    def test_sem_crescimento_nao_inventa_esgotamento(self):
        now = 10_000
        samples = [
            {"ts": now - 1800, "codex": 40},
            {"ts": now - 900, "codex": 40},
            {"ts": now, "codex": 40},
        ]
        result = qs.calculate_projection(samples, "codex", now, 40, 120)
        self.assertIsNone(result["projection_min"])
        self.assertEqual(result["projected_pct_at_reset"], 40)

class LimitsAndUtilTests(unittest.TestCase):
    def test_fallback_claude_preserva_percentual_oficial_do_cache(self):
        local = {
            "session_pct": 0, "week_pct": 26,
            "session_reset_min": None, "week_reset_min": 100,
            "source": "logs",
        }
        cached = {
            "session_pct": 10, "week_pct": 2,
            "session_reset_min": 40, "week_reset_min": 500,
            "source": "cache", "stale": True,
        }
        result = qs.merge_claude_fallback(local, cached)
        self.assertEqual(result["session_pct"], 10)
        self.assertEqual(result["session_reset_min"], 40)
        self.assertEqual(result["week_pct"], 2)
        self.assertEqual(result["week_reset_min"], 500)
        self.assertEqual(result["source"], "cache+logs")
    def test_load_limits_com_arquivo_corrompido_cai_no_default(self):
        tmpdir = tempfile.mkdtemp()
        try:
            path = os.path.join(tmpdir, "limits.json")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("{ nao é json")
            result = qs.load_limits(path=path)
            self.assertEqual(result, qs._DEFAULT_LIMITS)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_load_limits_com_arquivo_ausente_cai_no_default(self):
        result = qs.load_limits(path="/caminho/que/nao/existe/limits.json")
        self.assertEqual(result, qs._DEFAULT_LIMITS)

    def test_clamp_pct_limites(self):
        self.assertEqual(qs.clamp_pct(-5), 0)
        self.assertEqual(qs.clamp_pct(150), 100)
        self.assertEqual(qs.clamp_pct(42.6), 43)
        self.assertEqual(qs.clamp_pct(None), 0)


class ClaudeSnapshotTests(unittest.TestCase):
    def test_snapshot_preserva_apenas_reset_ainda_vigente(self):
        tmpdir = tempfile.mkdtemp()
        try:
            path = os.path.join(tmpdir, "claude-snapshot.json")
            now = datetime(2026, 9, 25, 12, 0, tzinfo=timezone.utc)
            qs.save_claude_api_snapshot({
                "session_pct": 31,
                "week_pct": 20,
                "session_reset_epoch": int((now + timedelta(minutes=80)).timestamp()),
                "week_reset_epoch": int((now - timedelta(minutes=1)).timestamp()),
                "observed_epoch": int(now.timestamp()),
            }, path=path)
            result = qs.load_claude_api_snapshot(now=now, path=path)
            self.assertEqual(result["session_reset_min"], 80)
            self.assertNotIn("week_reset_min", result)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)

    def test_snapshot_so_completa_horario_ausente(self):
        current = {"session_reset_min": None, "week_reset_min": 20, "source": "cache+logs"}
        snapshot = {
            "session_reset_min": 90,
            "session_reset_epoch": 1234,
            "week_reset_min": 100,
            "week_reset_epoch": 5678,
        }
        result = qs.merge_claude_reset_snapshot(current, snapshot)
        self.assertEqual(result["session_reset_min"], 90)
        self.assertEqual(result["session_reset_epoch"], 1234)
        self.assertEqual(result["week_reset_min"], 20)
        self.assertEqual(result["source"], "cache+logs+snapshot")


if __name__ == "__main__":
    unittest.main()
