# Building DDM

DDM is built on Linux with MinGW-w64 into one 32-bit Windows program that runs on Windows 7 to 11. There is no
Visual Studio project; `build.sh` is the whole build.

## What you need

| | Version used for the official builds |
|---|---|
| MinGW-w64, i686, win32 threads (`i686-w64-mingw32-g++`) | GCC 13 |
| Python 3, `zip` | any |
| NSIS (`makensis`), only for the setup | 3.x |
| `osslsigncode`, `openssl`, only for signing | any |

On Debian or Ubuntu: `sudo apt install g++-mingw-w64-i686 mingw-w64-tools python3 zip nsis cmake ninja-build`.

## The libraries

`build.sh` expects the libraries, built for i686 MinGW as static libraries, under `third_party/prefix`
(`include/` and `lib/`), and Boost's headers under `third_party/boost`. They are not stored in this repository.

| Library | Version | What DDM uses it for | Licence |
|---|---|---|---|
| [curl](https://curl.se) | 8.x | HTTP, HTTPS, FTP | curl (MIT-like) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | 3.6 | TLS for curl (works on Windows 7), signatures | Apache-2.0 |
| [libtorrent](https://libtorrent.org) | 2.0 | torrents and magnet links | BSD-3-Clause |
| [OpenSSL](https://www.openssl.org) | 3.5 | used by libtorrent | Apache-2.0 |
| [Boost](https://www.boost.org) | 1.90 (headers) | used by libtorrent | BSL-1.0 |
| [miniz](https://github.com/richgel999/miniz) | 3.x | ZIP | MIT |
| LZMA SDK (`third_party/lzma`, included) | | 7z archives' table of contents | public domain |

Each one builds with its own CMake (or, for OpenSSL, `Configure mingw`) and the toolchain file
`third_party/mingw32.cmake`, installed with `--prefix third_party/prefix`. The options that matter:

- **Mbed TLS**: `-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF`
- **curl**: `-DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_SCHANNEL=OFF -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_TARGET_WINDOWS_VERSION=0x0601`
- **OpenSSL**: `./Configure mingw no-shared no-tests no-apps --cross-compile-prefix=i686-w64-mingw32- -D_WIN32_WINNT=0x0601`
- **libtorrent**: `-DBUILD_SHARED_LIBS=OFF -Dstatic_runtime=ON -Ddeprecated-functions=OFF -DCMAKE_CXX_FLAGS="-D_WIN32_WINNT=0x0601"` with `BOOST_ROOT` pointing at Boost
- **miniz**: `-DBUILD_SHARED_LIBS=OFF`

**ffmpeg.exe** (joins a video's picture and sound, makes MP3s) is a separate program copied from
`third_party/bin/ffmpeg.exe`. `third_party/bin/ffmpeg-build.txt` has the exact configure line of the LGPL build DDM
ships; any FFmpeg for 32-bit Windows works for trying things out.

## Build

```sh
./build.sh        # out/DDM.exe, out/ddmhost.exe, out/ffmpeg.exe, out/extension/, the add-on zips
./build.sh test   # out/enginetest.exe (the download engine's tests; run it on Windows or under Wine)
```

`build.sh` compiles every `.cpp` under `src/`, and makes `src/common/addon_files.hpp` (the fingerprints of the
browser add-on's files) and `res/helpres.rc` (the help and the languages) by itself.

The setup: `cd installer && makensis -DVER=4.3.3 installer.nsi` (or `sh installer/release.sh`, which also signs
when it is given the signing files).

## The layout

| Folder | What is in it |
|---|---|
| `src/common` | small helpers: JSON, text, files, version |
| `src/core` | the engine, without any window: downloads, streams (HLS/DASH), sites, torrents, MEGA, security checks, settings |
| `src/ui` | every window, drawn with the Win32 API and GDI/GDI+ |
| `src/app`, `src/host` | the program's start, and `ddmhost.exe` (the browsers talk to DDM through it) |
| `extension` | the browser add-ons: `shared/` is the code, `chromium/` and `firefox/` hold each browser's manifest |
| `installer` | the NSIS setup, the release script, the site rules (`sites.json`) and the third-party notices |
| `res` | icon, help text and pictures, languages, the background-removal model |
| `tools` | the translation tools and texts (`tools/tr/<language>.txt`) |
| `tests` | the engine's and the video engine's tests |

## Signatures: what a build of your own does differently

Official releases are signed with DedSec's keys, which are **not** in this repository and never will be:

- `DDM.exe`, `ddmhost.exe` and the setup carry DedSec's Authenticode signature.
- `update.json`, `sites.json` and `DDM.exe.sig` are signed with DedSec's private key; the matching **public** key is
  in the source (`src/core/crypto.cpp`), so every DDM can check them.

A DDM you build yourself has no `DDM.exe.sig` and runs normally. It still checks for DedSec's official updates and
accepts only updates signed by DedSec. If you publish your own version, change `DDM_UPDATE_REPO` in
`src/common/version.hpp` and the public key to your own, and give it another name so nobody mistakes it for the
official DDM.

## Translations

`tools/strings.txt` and `tools/strings-extra.txt` list every text of the program; `tools/tr/<language>.txt` holds
each language's translations. After changing them: `python3 tools/i18n_merge.py ar bn es fr hi id pt-BR ur`.
