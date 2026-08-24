@echo off
rem run_jade_gui.bat [mod.jmod] — launch the native toolkit GUI.
set PATH=C:\Qt\6.11.1\mingw_64\bin;C:\Qt\Tools\mingw1310_64\bin;C:\Users\test\Desktop\tinkering\vtk\install\bin;%PATH%
start "" "%~dp0..\build-gui\app\jade_gui.exe" %*
