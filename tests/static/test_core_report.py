"""Compile the core reporter and verify its artifacts without launching the app."""
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import pytest

from conftest import WORKSPACE_ROOT


@pytest.fixture(scope="module", params=["x64", "x86"])
def reporter(request, tmp_path_factory):
    if os.name != "nt":
        pytest.skip("Requires the Windows C++ toolchain")
    work = tmp_path_factory.mktemp("core_report_" + request.param)
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = subprocess.check_output([str(vswhere), "-latest", "-property", "installationPath"], text=True).strip()
    source = work / "fixture.cpp"
    source.write_text(r'''
#include "CoreTestReport.h"
#include <cstdlib>
static int Print(core_test::Report& report, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = report.Print(format, args);
    va_end(args);
    return result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 3;
    core_test::Report report;
    int checks = 0, failures = 0;
    report.Run("pass", checks, failures, [&] { ++checks; });
    report.Run("fail<&\"'", checks, failures, [&] {
        ++checks; ++failures;
        Print(report, "%s %d\n", "failure <tag> & \"quoted\" 'apostrophe'\n日本語\x01", 42);
    });
    report.Run("skip", checks, failures, [&] { report.Skip("skip", "needs <option> & setup"); });
    report.Run("partial", checks, failures, [&] {
        ++checks;
        report.Skip("partial/optional", "unavailable");
    });
    report.Run("exception", checks, failures, [] { throw std::runtime_error("broken <state>"); });
    report.Run("fail-and-skip", checks, failures, [&] {
        ++failures;
        report.Skip("fail-and-skip", "skip must not hide failure");
    });
    try { report.Write(argv[1], sizeof(void*) == 8 ? "x64" : "x86"); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
    return failures ? 1 : 0;
}
''', encoding="utf-8")
    build = work / "build.cmd"
    build.write_text(
        f'@call "{vs}\\VC\\Auxiliary\\Build\\vcvarsall.bat" {request.param}\n'
        f'@if errorlevel 1 exit /b 1\n'
        f'@cl /nologo /utf-8 /std:c++17 /EHsc /W4 /I "{WORKSPACE_ROOT / "porting/tests/core"}" fixture.cpp /Fe:fixture.exe\n',
        encoding="ascii",
    )
    compiled = subprocess.run(["cmd.exe", "/c", str(build)], cwd=work, capture_output=True, timeout=120)
    assert compiled.returncode == 0, compiled.stdout.decode(errors="replace") + compiled.stderr.decode(errors="replace")
    return work / "fixture.exe", request.param


def test_report_results_and_escaping(reporter, tmp_path):
    executable, arch = reporter
    destination = tmp_path / "日本語 reports"
    run = subprocess.run([str(executable), str(destination)], capture_output=True, timeout=30)
    assert run.returncode == 1
    suite = ET.parse(destination / "report.xml").getroot().find("testsuite")
    assert suite.attrib["tests"] == "7"
    assert suite.attrib["failures"] == "3"
    assert suite.attrib["errors"] == "0"
    assert suite.attrib["skipped"] == "2"
    assert float(suite.attrib["time"]) >= 0
    props = {p.attrib["name"]: p.attrib["value"] for p in suite.findall("properties/property")}
    assert props == {"arch": arch, "checks": "3"}
    cases = {c.attrib["name"]: c for c in suite.findall("testcase")}
    assert cases["pass"].find("failure") is None
    assert cases['fail<&"\''].find("failure").text == 'failure <tag> & "quoted" \'apostrophe\'\n日本語? 42\n'
    assert b"42\n" in run.stdout.replace(b"\r\n", b"\n")
    assert cases["skip"].find("skipped").attrib["message"] == "needs <option> & setup"
    assert cases["partial"].find("skipped") is None
    assert cases["partial/optional"].find("skipped") is not None
    assert "broken <state>" in cases["exception"].find("failure").text
    assert cases["fail-and-skip"].find("failure") is not None
    assert cases["fail-and-skip"].find("skipped") is None
    html = (destination / "report.html").read_text(encoding="utf-8")
    assert "日本語?" in html
    assert "&lt;tag&gt; &amp; &quot;quoted&quot;" in html
    assert html.count('class="FAIL"') == 3
    assert html.count('class="SKIP"') == 2
    assert "<script" not in html


def test_report_write_failure(reporter, tmp_path):
    executable, _ = reporter
    destination = tmp_path / "reports"
    destination.mkdir()
    (destination / "report.xml").mkdir()
    run = subprocess.run([str(executable), str(destination)], capture_output=True, timeout=30)
    assert run.returncode == 2
    assert b"Cannot write core test report:" in run.stderr
