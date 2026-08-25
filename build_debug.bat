@echo off
rem Debug build (default x64). Pass a platform as arg 1 to override (e.g. build_debug.bat Win32).
call "%~dp0build.bat" Debug %1
exit /b %ERRORLEVEL%
