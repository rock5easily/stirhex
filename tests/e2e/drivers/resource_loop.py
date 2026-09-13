"""Repeat one editor operation and judge whether it leaks resources (Issue #224).

The shape of every stability test is the same: warm up so that one-off allocations (the
first paint of a bar, a dialog template, a font) are already charged to the process, then
repeat the operation and watch the counters. What is left over after the warm-up and does
not come back is a leak.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from drivers.resource_probe import (
    ProcessGoneError,
    ResourceProbe,
    ResourceSample,
    format_samples_csv,
    slope_per_iteration,
)

# One operation may legitimately move memory (undo history, caches, heap free lists), so
# memory is judged by its per-iteration trend rather than by a strict zero delta.
DEFAULT_MEMORY_SLOPE_LIMIT = 64 * 1024  # bytes per iteration


@dataclass
class LoopReport:
    """Counters recorded while one operation was repeated."""

    label: str
    iterations: int
    warmup: int
    baseline: ResourceSample
    samples: list[tuple[int, ResourceSample]] = field(default_factory=list)

    @property
    def final(self) -> ResourceSample:
        return self.samples[-1][1] if self.samples else self.baseline

    def delta(self) -> dict[str, int]:
        """Counter movement between the post-warm-up baseline and the last sample."""
        return self.final.delta(self.baseline)

    def slope(self, counter: str) -> float:
        """Per-iteration trend of one counter over the measured samples."""
        xs = [i for i, _ in self.samples]
        ys = [float(getattr(sample, counter)) for _, sample in self.samples]
        return slope_per_iteration(xs, ys)

    def half_growth(self, counter: str) -> tuple[int, int]:
        """Growth of one counter in the first and in the second half of the loop.

        Counters step up once for reasons that are not leaks: a cache fills, a font is
        realized, a dialog template is loaded, Windows grows its thread pool. Such a step
        lands in one half and leaves the other flat. A leak is charged per operation, so it
        shows up in both halves - which is what the verdict uses (Issue #224).
        """
        if len(self.samples) < 3:
            return 0, self.delta()[counter]
        mid = len(self.samples) // 2
        start = getattr(self.baseline, counter)
        middle = getattr(self.samples[mid][1], counter)
        end = getattr(self.final, counter)
        return middle - start, end - middle

    def summary(self) -> str:
        delta = self.delta()
        parts = [
            f"{self.label}: {self.iterations} iterations after {self.warmup} warm-up",
            f"  gdi {self.baseline.gdi_objects} -> {self.final.gdi_objects} "
            f"({delta['gdi_objects']:+d})",
            f"  user {self.baseline.user_objects} -> {self.final.user_objects} "
            f"({delta['user_objects']:+d})",
            f"  handles {self.baseline.handles} -> {self.final.handles} "
            f"({delta['handles']:+d})",
            f"  threads {self.baseline.threads} -> {self.final.threads} "
            f"({delta['threads']:+d})",
            f"  private bytes {self.baseline.private_bytes / 1024:.0f} KiB -> "
            f"{self.final.private_bytes / 1024:.0f} KiB "
            f"({delta['private_bytes'] / 1024:+.0f} KiB, "
            f"{self.slope('private_bytes') / 1024:+.2f} KiB/iteration)",
        ]
        halves = ", ".join(
            f"{counter} {self.half_growth(counter)[0]:+d}/{self.half_growth(counter)[1]:+d}"
            for counter in ResourceSample.EXACT_COUNTERS
        )
        parts.append(f"  growth per half (first/second): {halves}")
        return "\n".join(parts)

    def write_csv(self, directory: Path) -> Path:
        """Write every sample to <directory>/<label>.csv and return the path."""
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / f"{self.label}.csv"
        rows = [(0, self.baseline)] + self.samples
        path.write_text(format_samples_csv(rows), encoding="utf-8")
        return path


def run_operation_loop(
    pid: int,
    operation: Callable[[int], None],
    *,
    label: str,
    iterations: int,
    warmup: int = 5,
    sample_every: int = 1,
    settle: float = 0.4,
) -> LoopReport:
    """Repeat `operation` and sample the process counters around it.

    `operation` receives the 1-based iteration number and must leave the editor in the
    state it found it in, so that iteration N+1 does the same work as iteration N.
    `settle` is the pause before each sample: closing a window frees its GDI objects a
    moment after the call returns, and sampling too early would report a phantom leak.
    """
    if iterations < 2:
        raise ValueError("iterations must be at least 2 to see a trend")

    with ResourceProbe(pid) as probe:
        for i in range(1, warmup + 1):
            operation(i)
        time.sleep(settle)
        baseline = probe.sample()

        report = LoopReport(
            label=label, iterations=iterations, warmup=warmup, baseline=baseline
        )
        for i in range(1, iterations + 1):
            operation(warmup + i)
            if i % sample_every == 0 or i == iterations:
                time.sleep(settle)
                report.samples.append((i, probe.sample()))
        return report


def check_no_leak(
    report: LoopReport,
    *,
    memory_slope_limit: int = DEFAULT_MEMORY_SLOPE_LIMIT,
    allowed_growth: dict[str, int] | None = None,
) -> list[str]:
    """Return one message per counter that grew beyond what is allowed; empty when clean.

    `allowed_growth` raises the bar for a single counter when the operation is known to
    keep something alive on purpose (a document window that stays open, for instance).
    """
    allowed = allowed_growth or {}
    delta = report.delta()
    failures: list[str] = []

    for counter in ResourceSample.EXACT_COUNTERS:
        limit = allowed.get(counter, 0)
        first, second = report.half_growth(counter)
        # Both halves must have grown for this to be charged per operation rather than a
        # one-off step, and the total must exceed what the caller allows.
        if delta[counter] > limit and first > 0 and second > 0:
            failures.append(
                f"{counter} grew in both halves of the loop: {first:+d} then {second:+d} "
                f"({delta[counter]:+d} in total over {report.iterations} iterations, "
                f"allowed {limit}; {report.slope(counter):+.3f} per iteration)"
            )

    memory_slope = report.slope("private_bytes")
    if memory_slope > memory_slope_limit:
        failures.append(
            f"private_bytes grew by {memory_slope / 1024:.1f} KiB per iteration "
            f"(limit {memory_slope_limit / 1024:.1f} KiB); "
            f"total {delta['private_bytes'] / 1024:+.0f} KiB"
        )
    return failures


def alive_or_fail(pid: int) -> None:
    """Raise ProcessGoneError if the editor died during the loop."""
    with ResourceProbe(pid) as probe:
        if not probe.is_alive():
            raise ProcessGoneError(f"Process {pid} exited during the loop")
