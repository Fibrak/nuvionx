@echo off
rem Sends your Nuvio for Windows setup to NuvioNX on the Switch.
rem The Switch IP is read from switchip.txt in this folder (edit it if your IP changes).
rem NuvioNX must be open on the Switch, on the same Wi-Fi.
powershell -ExecutionPolicy Bypass -File "%~dp0import-from-windows.ps1"
pause
