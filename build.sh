#!/bin/sh
# Native DDM build (32-bit: one exe for Windows 7 to 11).
set -e
cd "$(dirname "$0")"
P=third_party/prefix
CXX=i686-w64-mingw32-g++
OUT=${OUT:-out}
mkdir -p $OUT obj
CFLAGS="-std=c++17 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DCURL_STATICLIB -DNOMINMAX -I$P/include -Wall -Wno-unused-parameter -Wno-missing-field-initializers -ffunction-sections -fdata-sections"
LIBS="-L$P/lib -lcurl -lminiz -lwbemuuid -loleaut32 -lpowrprof -lmbedtls -lmbedx509 -lmbedcrypto -leverest -lp256m -lws2_32 -lwldap32 -lcrypt32 -lwintrust -lurlmon -lbcrypt -lsecur32 -liphlpapi -lwinhttp -lnormaliz -lshlwapi -lole32 -luuid -lshell32 -luser32 -lgdi32 -lgdiplus -lcomctl32 -lcomdlg32 -luxtheme -ldwmapi -ladvapi32 -lversion -lwinmm -limm32"
LDFLAGS="-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s"
# the built-in torrent engine (libtorrent + OpenSSL, built for Windows 7): only torrent.cpp includes it
LTFLAGS="-Ithird_party/boost -DBOOST_ASIO_ENABLE_CANCELIO -DBOOST_ASIO_NO_DEPRECATED -DTORRENT_NO_DEPRECATE -DTORRENT_USE_OPENSSL -DTORRENT_USE_LIBCRYPTO -DTORRENT_SSL_PEERS -DOPENSSL_NO_SSL2 -DOPENSSL_NO_SSL3 -DOPENSSL_NO_TLS1 -DOPENSSL_NO_TLS1_1 -DOPENSSL_NO_DTLS1"
LIBS="-L$P/lib -ltorrent-rasterbar -lssl -lcrypto -lmswsock $LIBS"
compile() { # src -> obj
  o=obj/$(echo "$1" | tr '/' '_' | sed 's/\.cpp$/.o/')
  x=""; case "$1" in src/core/torrent.cpp) x="$LTFLAGS" ;; src/core/bgremove.cpp) x="-O3 -msse2 -mfpmath=sse" ;; esac
  if [ ! -f "$o" ] || [ "$1" -nt "$o" ] || [ -n "$(find src -name '*.hpp' -newer "$o" 2>/dev/null | head -1)" ]; then $CXX $CFLAGS $x -c "$1" -o "$o"; fi
  echo "$o"
}
# the browser add-on, and the fingerprints of its files (DDM trusts a copy loaded by hand only when every file matches)
case "$OUT" in /*) EXTOUT="$OUT" ;; *) EXTOUT="$(pwd)/$OUT" ;; esac
sh extension/build.sh "$EXTOUT" >/dev/null
python3 - "$OUT" <<'PY'
import hashlib, json, os, sys
out = sys.argv[1]; d = os.path.join(out, 'extension', 'chromium')
files = {}
for n in sorted(os.listdir(d)):
    if n != 'manifest.json': files[n] = hashlib.sha256(open(os.path.join(d, n), 'rb').read()).hexdigest()
m = open(os.path.join(d, 'manifest.json'), 'rb').read()
j = json.loads(m); j.pop('key', None)
store = json.dumps(j, indent=2, ensure_ascii=False).encode('utf-8')
mans = sorted({hashlib.sha256(m).hexdigest(), hashlib.sha256(store).hexdigest()})
h = ['// Made by build.sh from out/extension/chromium: the SHA-256 of each file of DDM\'s own browser add-on.', '#pragma once', 'namespace ddm { namespace addonfiles {',
     'static const char* const kFiles[][2] = {' + ', '.join('{"%s", "%s"}' % (k, v) for k, v in files.items()) + '};',
     'static const char* const kManifests[] = {' + ', '.join('"%s"' % x for x in mans) + '};', '} }', '']
text = '\n'.join(h)
p = 'src/common/addon_files.hpp'
if not os.path.exists(p) or open(p).read() != text: open(p, 'w').write(text)
PY
CORE=""
for f in src/common/*.cpp src/core/*.cpp; do CORE="$CORE $(compile $f)"; done
# the LZMA decoder (7z archives' table of contents)
for f in third_party/lzma/LzmaDec.c third_party/lzma/Lzma2Dec.c; do o=obj/lzma_$(basename $f .c).o; if [ ! -f $o ] || [ $f -nt $o ]; then i686-w64-mingw32-gcc -O2 -c $f -o $o; fi; CORE="$CORE $o"; done
i686-w64-mingw32-windres -I res res/core.rc -O coff -o obj/core_res.o
case "$1" in
  test) $CXX $CFLAGS tests/enginetest.cpp $CORE obj/core_res.o -o $OUT/enginetest.exe $LDFLAGS $LIBS -mconsole ;;
  *) UI=""; for f in src/ui/*.cpp src/app/*.cpp; do [ -f "$f" ] && UI="$UI $(compile $f)"; done
     # the built-in help: its text and pictures
     { echo 'DDMHELP RCDATA "help.txt"'; for f in res/help/*.png; do n=$(basename "$f" .png | tr a-z A-Z); echo "HLP_$n RCDATA \"help/$(basename "$f")\""; done
       for f in res/lang/*.txt; do [ -f "$f" ] || continue; n=$(basename "$f" .txt | tr a-z- A-Z_); echo "LANG_$n RCDATA \"lang/$(basename "$f")\""; done; } > res/helpres.rc
     i686-w64-mingw32-windres -I res res/ddm.rc -O coff -o obj/ddm_res.o
     $CXX $CORE $UI obj/ddm_res.o -o $OUT/DDM.exe $LDFLAGS $LIBS -mwindows -municode
     $CXX $CFLAGS src/host/main.cpp obj/src_common_json.o obj/src_common_util.o obj/src_core_ipc.o -o $OUT/ddmhost.exe $LDFLAGS -lshell32 -lole32 -luuid -ladvapi32 -mconsole -municode
     # the joiner for video + sound and MP3s (FFmpeg, LGPL build: see third_party/bin/ffmpeg-build.txt), installed next to DDM.exe
     cp third_party/bin/ffmpeg.exe $OUT/ffmpeg.exe ;;
esac
