/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

// This is an include file that sets up the Windows version definitions.

#ifndef SDL_winver_h_
#define SDL_winver_h_

#include <SDL3/SDL_platform_defines.h>

#if defined(SDL_PLATFORM_WIN32)
#ifdef WINVER
#undef WINVER
#endif
#ifdef _WIN32_MAXVER
#undef _WIN32_MAXVER
#endif

/* winsdkver.h defines _WIN32_MAXVER for SDK version detection. It is present since at least the Windows 7 SDK,
 * but out of caution we'll only use it if the compiler supports __has_include() to confirm its presence.
 * If your compiler doesn't support __has_include() but you have winsdkver.h, define HAVE_WINSDKVER_H.  */
#if !defined(HAVE_WINSDKVER_H) && defined(__has_include)
#if __has_include(<winsdkver.h>)
#define HAVE_WINSDKVER_H 1
#endif
#endif

#ifdef HAVE_WINSDKVER_H
#include <winsdkver.h>
#endif

/* sdkddkver.h defines more specific SDK version numbers. This is needed because older versions of the
 * Windows 10 SDK have broken declarations for the C API for DirectX 12. */
#if !defined(HAVE_SDKDDKVER_H) && defined(__has_include)
#if __has_include(<sdkddkver.h>)
#define HAVE_SDKDDKVER_H 1
#endif
#endif

#ifdef HAVE_SDKDDKVER_H
#include <sdkddkver.h>
#endif

#if !defined(HAVE_SDKDDKVER_H) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT  0x0501
#endif
#if !defined(HAVE_WINSDKVER_H) && !defined(_WIN32_MAXVER)
#define _WIN32_MAXVER _WIN32_WINNT
#endif
#if !defined(HAVE_SDKDDKVER_H)
#define WINVER        _WIN32_WINNT
#endif

#elif defined(SDL_PLATFORM_WINGDK) || defined(SDL_PLATFORM_XBOXONE) || defined(SDL_PLATFORM_XBOXSERIES)
#undef _WIN32_MAXVER
#undef _WIN32_WINNT
#undef WINVER
#define _WIN32_WINNT  0x0A00
#define _WIN32_MAXVER _WIN32_WINNT
#define WINVER        _WIN32_WINNT
#endif

#ifndef _WIN32_WINNT_NT4
#define _WIN32_WINNT_NT4     0x0400
#endif
#ifndef _WIN32_WINNT_WIN2K
#define _WIN32_WINNT_WIN2K   0x0500
#endif
#ifndef _WIN32_WINNT_WINXP
#define _WIN32_WINNT_WINXP   0x0501
#endif
#ifndef _WIN32_WINNT_WS03
#define _WIN32_WINNT_WS03    0x0502
#endif
#ifndef _WIN32_WINNT_VISTA
#define _WIN32_WINNT_VISTA   0x0600
#endif
#ifndef _WIN32_WINNT_WIN7
#define _WIN32_WINNT_WIN7    0x0601
#endif
#ifndef _WIN32_WINNT_WIN8
#define _WIN32_WINNT_WIN8    0x0602
#endif
#ifndef _WIN32_WINNT_WINBLUE
#define _WIN32_WINNT_WINBLUE 0x0603
#endif
#ifndef _WIN32_WINNT_WIN10
#define _WIN32_WINNT_WIN10   0x0A00
#endif

#endif // SDL_winver_h_
