@echo off
rem Release build (default x64). Pass a platform as arg 1 to override (e.g. build_release.bat Win32).
call "%~dp0build.bat" Release %1
exit /b %ERRORLEVEL%
