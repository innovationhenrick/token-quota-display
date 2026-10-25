import unittest
from quota_server import calculate_projection, minutes_until
from datetime import datetime, timezone


class StageOneTests(unittest.TestCase):
    def test_projection_does_not_mix_windows(self):
        samples = [
            {"ts": 1000, "claude": 80}, {"ts": 1600, "claude": 90},
            {"ts": 1700, "claude": 5}, {"ts": 1800, "claude": 6},
        ]
        self.assertIsNone(calculate_projection(samples, "claude", 1800, 6, 280)["projection_min"])

    def test_short_history_is_not_a_confident_prediction(self):
        samples = [{"ts": t, "claude": p} for t, p in [(1000, 5), (1100, 10), (1200, 15)]]
        self.assertIsNone(calculate_projection(samples, "claude", 1200, 15, 280)["projected_pct_at_reset"])

    def test_absolute_reset_survives_repeated_cache_reads(self):
        reset = 7200
        self.assertEqual(minutes_until(reset, datetime.fromtimestamp(3600, timezone.utc)), 60)
        self.assertEqual(minutes_until(reset, datetime.fromtimestamp(3660, timezone.utc)), 59)

    def test_expired_reset_never_negative(self):
        self.assertEqual(minutes_until(1000, datetime.fromtimestamp(2000, timezone.utc)), 0)
