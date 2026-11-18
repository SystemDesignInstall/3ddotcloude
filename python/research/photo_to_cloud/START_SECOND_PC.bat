@echo off
chcp 65001 >nul
"%~dp0SpatialPhotoCloud.exe" --lan-node %*
pause
