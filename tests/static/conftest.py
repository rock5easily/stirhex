"""Shared paths for the static checks (Issue #206).

These tests read files from the repository; they never start an application. They used to
live under tests/e2e/tests/issues, where the shared conftest pulls in Win32 dependencies,
kills stray editor processes at session start and rewrites the application settings around
every test - none of which a documentation check needs.
"""
from pathlib import Path

# porting/tests/static/conftest.py -> repository root
WORKSPACE_ROOT = Path(__file__).resolve().parents[3]
