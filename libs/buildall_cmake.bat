@echo off
setlocal
cd /d %~dp0

if NOT "%CMAKE%" == "" goto pass_set_cmake
rem set CMAKE=cmake.exe
set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
rem set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
rem set CMAKE="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set OPT=
:pass_set_cmake

:retry_vs
echo 1. Visual Studio 17 2022 Win32
echo 2. Visual Studio 17 2022 x64
echo 3. Visual Studio 16 2019 Win32
echo 4. Visual Studio 16 2019 x64
echo 5. Visual Studio 15 2017
echo 6. Visual Studio 14 2015
echo 7. Visual Studio 12 2013
echo 8. Visual Studio 11 2012
echo 9. Visual Studio 10 2010
echo a. Visual Studio 9 2008
echo b. Visual Studio 8 2005
rem echo c. Visual Studio NMake (experimental)
rem echo d. Cygwin MinGW Release + Unix Makefiles (experimental)
set /p no="select no "

echo %no%
if "%no%" == "1" set GENERATOR="Visual Studio 17 2022" & set OPT=-DARCHITECTURE=Win32 & goto build_all
if "%no%" == "2" set GENERATOR="Visual Studio 17 2022" & set OPT=-DARCHITECTURE=x64 & goto build_all
if "%no%" == "3" set GENERATOR="Visual Studio 16 2019" & set OPT=-DARCHITECTURE=Win32 & goto build_all
if "%no%" == "4" set GENERATOR="Visual Studio 16 2019" & set OPT=-DARCHITECTURE=x64 & goto build_all
if "%no%" == "5" set GENERATOR="Visual Studio 15 2017" & goto build_all
if "%no%" == "6" set GENERATOR="Visual Studio 14 2015" & goto build_all
if "%no%" == "7" set GENERATOR="Visual Studio 12 2013" & goto build_all
if "%no%" == "8" set GENERATOR="Visual Studio 11 2012" & goto build_all
if "%no%" == "9" set GENERATOR="Visual Studio 10 2010" & goto build_all
if "%no%" == "a" set GENERATOR="Visual Studio 9 2008" & goto build_all
if "%no%" == "b" set GENERATOR="Visual Studio 8 2005" & call :cmake_3_11_4 & goto build_all_2
if "%no%" == "c" set GENERATOR="NMake Makefiles" & set OPT=-DCMAKE_BUILD_TYPE=Release & goto build_all
if "%no%" == "d" set GENERATOR="Unix Makefiles" & set OPT=-DCMAKE_TOOLCHAIN_FILE=../mingw.toolchain.cmake & goto build_all
echo ? retry
goto retry_vs

:build_all
if exist %CMAKE% goto build_all_2
where %CMAKE% 2> nul
if %errorlevel% == 0 goto build_all_2
echo cmake not found
pause
exit

:build_all_2
set C=%CMAKE% -DCMAKE_GENERATOR=%GENERATOR% %OPT% -P buildall.cmake
echo %C%
title %C%
pause
%C%

:finish
echo build complete
endlocal
pause
exit

:cmake_3_11_4
set CMAKE=%~dp0..\buildtools\cmake-3.11.4-win32-x86\bin\cmake.exe
echo 1. PATH上のcmake.exeを使用する
echo 2. VS2005でも使用できるcmake使用する
echo    (必要なら自動でダウンロードして、このbuildtools/にインストールする)
if exist %CMAKE% echo    インストール済み(%CMAKE%)

set /p no="select no "
echo %no%
if "%no%" == "2" goto download

set CMAKE="cmake.exe"
where %CMAKE%
goto finish_cmake

:download
if exist %CMAKE% goto finish_cmake
call ..\buildtools\getcmake.bat

:finish_cmake
exit /b
