"""Fixtures for the resource leak tests (Issue #224).

Every test here drives StirHex only, so it carries the `ported` mark like any other
port-only test. What sets it apart is cost, not the executable it starts: each test
repeats one operation dozens of times, so the whole directory is skipped unless
`--stability` is passed.
"""

from pathlib import Path

import pytest

from drivers.resource_loop import (
    LoopReport,
    check_no_leak,
    run_operation_loop,
)

REPORT_DIR = Path(__file__).resolve().parents[2] / "reports" / "stability"


@pytest.fixture(autouse=True)
def require_stability_option(request):
    if not request.config.getoption("--stability"):
        pytest.skip("stability tests run only with --stability")


@pytest.fixture
def stability_iterations(request) -> int:
    return request.config.getoption("--stability-iterations")


@pytest.fixture
def stability_warmup(request) -> int:
    return request.config.getoption("--stability-warmup")


@pytest.fixture
def leak_check(request, stability_iterations, stability_warmup):
    """Repeat an operation, record the counters, and fail when they keep climbing.

    The report is written to reports/stability/<label>.csv and printed either way, so a
    run that passes still leaves the numbers behind to compare against later.
    """

    def _run(
        pid: int,
        operation,
        *,
        label: str | None = None,
        iterations: int | None = None,
        warmup: int | None = None,
        settle: float = 0.4,
        memory_slope_limit: int | None = None,
        allowed_growth: dict[str, int] | None = None,
    ) -> LoopReport:
        report = run_operation_loop(
            pid,
            operation,
            label=label or request.node.name,
            iterations=iterations if iterations is not None else stability_iterations,
            warmup=warmup if warmup is not None else stability_warmup,
            settle=settle,
        )
        csv_path = report.write_csv(REPORT_DIR)
        print(f"\n{report.summary()}\n  samples: {csv_path}")

        kwargs = {"allowed_growth": allowed_growth}
        if memory_slope_limit is not None:
            kwargs["memory_slope_limit"] = memory_slope_limit
        failures = check_no_leak(report, **kwargs)
        if failures:
            detail = "\n".join(f"  - {f}" for f in failures)
            pytest.fail(f"{report.label} leaks resources:\n{detail}\n{report.summary()}")
        return report

    return _run
