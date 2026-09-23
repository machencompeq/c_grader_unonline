@echo off
rem Rebuild Grader.exe (close the Grader window first).
cd /d "%~dp0"
if exist build\Grader_next.exe (
    move /y build\Grader_next.exe Grader.exe >nul
)
mingw32-make all test
if errorlevel 1 (
    echo.
    echo Build failed. Is Grader.exe still open?
)
pause
