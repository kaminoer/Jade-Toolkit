@echo off
rem run_vtk_spike.bat <bf> <zone_substring> — interactive D1 spike viewer.
rem LMB-drag orbit, wheel zoom, F toggles fly, click prints pick timing.
set PATH=C:\Qt\6.11.1\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;C:\Users\test\Desktop\tinkering\vtk\install\bin;%PATH%
"%~dp0..\build-gui\tools\vtk_spike.exe" %*
