@echo off
cd /d "%~dp0"
where node >nul 2>&1
if errorlevel 1 (
    echo Node.js is required to start Scheduler Workbench.
    pause
    exit /b 1
)
node web\server.mjs --open
if errorlevel 1 pause
