"""The leak verdict itself, checked against synthetic samples (Issue #224).

A leak test that can never fail is worse than no test, so the rule that decides "leak" or
"no leak" is exercised here on made-up counter sequences: a steady per-operation leak must
be reported, and the one-off steps a healthy process makes must not be.

These start no executable, so they carry the `unit` mark and run in the normal suite.
"""

import pytest

from drivers.resource_loop import LoopReport, check_no_leak
from drivers.resource_probe import ResourceSample, slope_per_iteration

pytestmark = pytest.mark.unit


def _sample(index: int, *, handles=300, gdi=80, user=78, threads=6, private=4_000_000):
    return ResourceSample(
        timestamp=float(index),
        private_bytes=private,
        working_set=private * 8,
        gdi_objects=gdi,
        user_objects=user,
        handles=handles,
        threads=threads,
    )


def _report(counters: list[dict], label="synthetic") -> LoopReport:
    """Build a report whose baseline is counters[0] and whose samples are the rest."""
    baseline = _sample(0, **counters[0])
    report = LoopReport(
        label=label,
        iterations=len(counters) - 1,
        warmup=5,
        baseline=baseline,
    )
    report.samples = [
        (i, _sample(i, **values)) for i, values in enumerate(counters[1:], start=1)
    ]
    return report


class TestLeakVerdict:
    def test_flat_counters_pass(self):
        report = _report([{} for _ in range(21)])
        assert check_no_leak(report) == []

    def test_handle_leak_per_iteration_is_reported(self):
        """One handle per iteration is the classic leak shape."""
        report = _report([{"handles": 300 + i} for i in range(21)])
        failures = check_no_leak(report)
        assert any("handles" in f for f in failures), failures

    def test_slow_gdi_leak_is_reported(self):
        """A leak of one object every few iterations still grows in both halves."""
        report = _report([{"gdi": 80 + i // 4} for i in range(21)])
        failures = check_no_leak(report)
        assert any("gdi_objects" in f for f in failures), failures

    def test_one_off_step_is_not_a_leak(self):
        """A single step - a cache filling, a thread pool growing - must not fail."""
        counters = [{} for _ in range(21)]
        for i in range(14, 21):
            counters[i] = {"threads": 8, "handles": 302}
        report = _report(counters)
        assert check_no_leak(report) == []

    def test_memory_growth_beyond_the_limit_is_reported(self):
        """Memory is judged by its slope, not by a single jump."""
        report = _report([{"private": 4_000_000 + i * 200 * 1024} for i in range(21)])
        failures = check_no_leak(report)
        assert any("private_bytes" in f for f in failures), failures

    def test_memory_growth_within_the_limit_passes(self):
        report = _report([{"private": 4_000_000 + i * 8 * 1024} for i in range(21)])
        assert check_no_leak(report) == []

    def test_allowed_growth_raises_the_bar_for_one_counter(self):
        """An operation that knowingly keeps something alive can declare it."""
        report = _report([{"handles": 300 + i // 10} for i in range(21)])
        assert any("handles" in f for f in check_no_leak(report))
        assert check_no_leak(report, allowed_growth={"handles": 2}) == []


class TestSlope:
    def test_slope_of_a_straight_line(self):
        assert slope_per_iteration([1, 2, 3, 4], [10.0, 20.0, 30.0, 40.0]) == pytest.approx(10.0)

    def test_slope_of_flat_values_is_zero(self):
        assert slope_per_iteration([1, 2, 3], [7.0, 7.0, 7.0]) == pytest.approx(0.0)

    def test_slope_needs_two_points(self):
        assert slope_per_iteration([1], [5.0]) == 0.0
