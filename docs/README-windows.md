# Windows

## Old systems

WinRT, Windows Phone, and UWP are no longer supported.

All desktop Windows versions, back to Windows XP, are still supported.

## LLVM and Intel C++ compiler support

SDL will build with the Visual Studio project files with LLVM-based compilers,
such as the Intel oneAPI C++ compiler, but you'll have to manually add the
"-msse3" command line option to at least the SDL\_audiocvt.c source file, and
possibly others. This may not be necessary if you build SDL with CMake instead
of the included Visual Studio solution.

Details are here: https://github.com/libsdl-org/SDL/issues/5186

## MinGW-w64 compiler support

SDL can be built with MinGW-w64 and CMake. Minimum tested MinGW-w64 version is 8.0.3.

On a Windows host, you first need to install and set up the MSYS2 environment, which provides the MinGW-w64 toolchain. Install MSYS2, typically to `C:\msys64`, and follow the instructions on the MSYS2 wiki to use the MinGW-w64 shell to update all components in the MSYS2 environment. This generally amounts to running `pacman -Syuu` from the mingw64 shell, but refer to MSYS2's documentation for more details. Once the MSYS2 environment has been updated, install the x86\_64 MinGW toolchain from the mingw64 shell with the command `pacman -S mingw-w64-x86_64-toolchain`. (You can additionally install `mingw-w64-i686-toolchain` if you intend to build 32-bit binaries as well. The remainder of this section assumes you only want to build 64-bit binaries.)

To build and install SDL, you can use PowerShell or any CMake-compatible IDE. First, install CMake, Ninja, and Git. These tools can be installed using any number of tools, such as the MSYS2's `pacman`, `winget`, `Chocolatey`, or by manually downloading and running the installers. Clone SDL to an appropriate location with `git` and run the following commands from the root of the cloned repository:

```sh
mkdir build
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=build-scripts/cmake-toolchain-mingw64-x86_64.cmake
cmake --build build --parallel
cmake --install build --prefix C:/Libraries
```

This installs SDL to `C:\Libraries`. You can specify another directory of your choice as desired. Ensure that your `CMAKE_PREFIX_PATH` includes `C:\Libraries` when you want to build against this copy of SDL. The simplest way to do this is to pass it to CMake as an option at configuration time:

```sh
cmake .. -G Ninja -DCMAKE_PREFIX_PATH=C:/Libraries
```

You will also need to configure CMake to use the MinGW-w64 toolchain to build your own project. Here is a minimal toolchain file that you could use for this purpose:

```
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

find_program(CMAKE_C_COMPILER NAMES x86_64-w64-mingw32-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES x86_64-w64-mingw32-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES x86_64-w64-mingw32-windres windres REQUIRED)
```

Save this in your project and refer to it at configuration time with the option `-DCMAKE_TOOLCHAIN_FILE`.

On Windows, you also need to copy `SDL3.dll` to an appropriate directory so that the game can find it at runtime. For guidance, see [README-cmake.md](README-cmake.md#how-do-i-copy-a-sdl3-dynamic-library-to-another-location).

Below is a minimal `CMakeLists.txt` file to build your game linked against a system SDL that was built with the MinGW-w64 toolchain. See [README-cmake.md](README-cmake.md) for more details on including SDL in your CMake project.

```cmake
cmake_minimum_required(VERSION 3.15)
project(mygame)

find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-shared)

add_executable(mygame WIN32 mygame.c)
target_link_libraries(mygame PRIVATE SDL3::SDL3)

# On Windows, copy SDL3.dll to the build directory
if(WIN32)
    add_custom_command(
        TARGET mygame POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:mygame>
        VERBATIM
    )
endif()
```

## OpenGL ES 2.x support

SDL has support for OpenGL ES 2.x under Windows via two alternative
implementations.

The most straightforward method consists in running your app in a system with a
graphic card paired with a relatively recent (as of November of 2013) driver
which supports the WGL\_EXT\_create\_context\_es2\_profile extension. Vendors
known to ship said extension on Windows currently include nVidia and Intel.

The other method involves using the
[ANGLE library](https://code.google.com/p/angleproject/). If an OpenGL ES 2.x
context is requested and no WGL\_EXT\_create\_context\_es2\_profile extension
is found, SDL will try to load the libEGL.dll library provided by ANGLE.

To obtain the ANGLE binaries, you can either compile from source from
https://chromium.googlesource.com/angle/angle or copy the relevant binaries
from a recent Chrome/Chromium install for Windows. The files you need are:

- libEGL.dll
- libGLESv2.dll
- d3dcompiler\_46.dll (supports Windows Vista or later, better shader
  compiler) *or* d3dcompiler\_43.dll (supports Windows XP or later)

If you compile ANGLE from source, you can configure it so it does not need the
d3dcompiler\_\* DLL at all (for details on this, see their documentation).
However, by default SDL will try to preload the d3dcompiler\_46.dll to comply
with ANGLE's requirements. If you wish SDL to preload d3dcompiler\_43.dll (to
support Windows XP) or to skip this step at all, you can use the
SDL\_HINT\_VIDEO\_WIN\_D3DCOMPILER hint (see SDL\_hints.h for more details).

Known Bugs:

- SDL\_GL\_SetSwapInterval is currently a no op when using ANGLE. It appears
  that there's a bug in the library which prevents the window contents from
  refreshing if this is set to anything other than the default value.

## Vulkan Surface Support

Support for creating Vulkan surfaces is configured on by default. To disable
it change the value of `SDL_VIDEO_VULKAN` to 0 in `SDL_config_windows.h`. You
must install the [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) in order to
use Vulkan graphics in your application.

## Transparent Window Support

SDL uses the Desktop Window Manager (DWM) to create transparent windows. DWM is
always enabled from Windows 8 and above. Windows 7 only enables DWM with Aero Glass
theme.

However, it cannot be guaranteed to work on all hardware configurations (an example
is hybrid GPU systems, such as NVIDIA Optimus laptops).

## Targeting Specific Windows Versions

If you're using a modern Windows toolchain, and only intend to support modern
Windows (Windows 10 and up), you don't need to worry about how Windows version
targeting works, it's all automatically handled for you. But, if you want to
target legacy versions of Windows (before Windows 10), there are additional
steps to get the build configured properly.

The two main things needed for targeting legacy Windows versions are defining
`_WIN32_WINNT` to the value corresponding to the version you're targeting, and
selecting a supported toolchain. It's likely *fine* to target an old version,
say Windows XP, and see your build works on all later versions, but there are
good reasons to have separate builds for each version of Windows (why is noted
below).

### Defining `_WIN32_WINNT`

Defining `_WIN32_WINNT` is the standard way you control which level of Windows
APIs your software will be built with. SDL's own source code checks it in
various places, adjusting which Windows APIs are used based on the value of
`_WIN32_WINNT`. How to define it is described below for each of the documented
toolchains. Each toolchain has its own default value of `_WIN32_WINNT`, so if
the version of Windows you're targeting isn't the default, you have to manually
define it.

Here are the values of `_WIN32_WINNT` that SDL supports:

- Windows XP: `0x0501`
- Windows Server 2003: `0x0502`
- Windows Vista: `0x0600`
- Windows 7: `0x0601`
- Windows 8: `0x0602`
- Windows 8.1: `0x0603`
- Windows 10 (and 11, at least): `0x0A00`

Note that merely choosing a low version value of `_WIN32_WINNT` but using a
modern toolchain might not produce builds that actually work on the version you
intended to target, with users seeing a "this is an invalid Win32 application"
error when they try to run it. Read below for how to avoid that issue.

### Toolchain: Visual Studio 2022 with CMake

Visual Studio 2022 can target all the way back to Windows XP, but requires
installing the component "C++ Windows XP Support for VS 2017 (v141) tools
[Deprecated]", which can be done in Visual Studio Installer. Also, users'
Windows XP machines need to have [the right Visual C++ redistributable installed](http://download.microsoft.com/download/9/3/F/93FCF1E7-E6A4-478B-96E7-D4B285925B00/vc_redist.x86.exe)
for builds from such a toolchain to work on their machine. You'll also probably
want the Visual Studio component "C++ CMake tools for Windows" installed, in
order to follow along with the CMake example; you can then directly use CMake
within a "Developer PowerShell for VS 2022" terminal environment.

You must use a Visual Studio CMake generator (passed to the `-G` CMake option),
not any others like Ninja, in order for the `-T` option to work (that's a
limitation with CMake itself). Passing in `-T v141_xp` selects the Windows XP
toolchain for building.

When using CMake with a Visual Studio generator, you select the CPU target with
the `-A` CMake option; `-A Win32` for 32-bit x86, `-A x64` for 64-bit x86. The
example here targets 32-bit x86, the typical choice for supporting Windows XP.

You provide `_WIN32_WINNT` to the configuration step via
`-DCMAKE_C_FLAGS=/D_WIN32_WINNT=0x....`. This example targets Windows XP, so we
use `-DCMAKE_C_FLAGS=/D_WIN32_WINNT=0x0501`.

SDL's own CMake option, `SDL_PRESEED`, is enabled by default, but has to be
manually disabled when targeting below Windows 10, or the configuration/build
won't work properly on the target Windows versions.

So, with your terminal's current working directory in the SDL source code's
root directory, run CMake like this:

```sh
mkdir build
cmake -S . -B build-xp -G "Visual Studio 17 2022" -T v141_xp -A Win32 -DCMAKE_C_FLAGS=/D_WIN32_WINNT=0x0501 -DSDL_PRESEED=OFF

# Debug builds
cmake --build build-xp --config Debug --parallel
cmake --install build-xp --config Debug --prefix C:/Libraries/DebugXP

# Release builds
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix C:/Libraries/ReleaseXP
```

### Why Provide Multiple Version-Specific Builds?

Some nice, or even necessary, Windows features are only available starting at
some minimum Windows version above Windows XP. For example, SDL\_gpu can only
be supported starting at Windows 7, as no modern graphics APIs are available
before Windows 7, required to support SDL\_gpu. Some features
supported/necessary in Windows 10 on modern machines, like high DPI support,
require targeting Windows 10. And a nice-to-have feature, high resolution
waitable timers (good for frame pacing) is only available on Windows 10 and up.
Ideally, you'd provide builds targeting each of the major versions that can
support what your software requires.
