@echo off
echo Generating solution and project files for Visual Studio 2022
pushd ..
zidar\tools\bin\windows\genie.exe --file=src\MTuner\scripts\genie.lua vs2022
popd
