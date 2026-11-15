@echo off
title Index voice server - keep this window open
cd /d "%~dp0"
echo Starting Index voice page at http://localhost:5190/
echo Keep this window open while you use it. Close it to stop.
start "" cmd /c "timeout /t 2 >nul & start http://localhost:5190/"
python -m http.server 5190
pause
