@echo off
rem Thin wrapper so publish.ps1 is one double-click away.
rem   publish.bat             -> build + stage + zip, opens _upload\
rem   publish.bat --no-zip    -> build + stage only (dev loop)
rem   publish.bat --no-open   -> do not open explorer when done
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish.ps1" %*
exit /b %ERRORLEVEL%
