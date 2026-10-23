@echo off
rem ESP8266 emulator: double-click, or run from any terminal. Builds and starts the GUI in WSL and opens
rem http://127.0.0.1:8266/ in your browser; drop a firmware.bin with its .elf on the page. Ctrl+C stops it.
rem Options go to run-gui.ps1, for example:  emulator.cmd -Port 8276 -Offline
rem emulator.cmd -Install adds the esp8266emu command, which does the same from any folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-gui.ps1" %*
if errorlevel 1 pause
