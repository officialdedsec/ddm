#!/bin/sh
# Release: build DDM, sign it (when the signing files are given), and make the setup and the browser add-ons.
#   DDM_SIGN_PFX / DDM_SIGN_PASS      Authenticode certificate (.pfx) and its password
#   DDM_SIGN_PFX2                     optional renewed certificate (same key, same password): added as a second signature
#   DDM_LICENSE_KEY_PEM               DedSec's private key: DDM.exe.sig, so a changed DDM.exe refuses to start
# None of these files belong in the source: keep them outside it.
set -e
cd "$(dirname "$0")/.."
./build.sh
sign_file() {
  [ -n "$DDM_SIGN_PFX" ] && [ -f "$DDM_SIGN_PFX" ] || { echo "not signed: $1"; return 0; }
  osslsigncode sign -pkcs12 "$DDM_SIGN_PFX" -pass "$DDM_SIGN_PASS" -h sha256 -n "DDM - DedSec Download Manager" -i "https://dedseec.com" \
    ${DDM_SIGN_TS:+-ts $DDM_SIGN_TS} -in "$1" -out "$1.signed" >/dev/null && mv "$1.signed" "$1" && echo "signed $1"
  # a second signature (renewed certificate, same key): added after the first, so DDM 4.0.4 and older still update
  if [ -n "$DDM_SIGN_PFX2" ] && [ -f "$DDM_SIGN_PFX2" ]; then
    osslsigncode sign -nest -pkcs12 "$DDM_SIGN_PFX2" -pass "$DDM_SIGN_PASS" -h sha256 -n "DDM - DedSec Download Manager" -i "https://dedseec.com" \
      ${DDM_SIGN_TS:+-ts $DDM_SIGN_TS} -in "$1" -out "$1.signed" >/dev/null && mv "$1.signed" "$1" && echo "  + second signature"
  fi
}
sign_file out/DDM.exe
sign_file out/ddmhost.exe
sign_file out/ffmpeg.exe
rm -f out/DDM.exe.sig
if [ -n "$DDM_LICENSE_KEY_PEM" ] && [ -f "$DDM_LICENSE_KEY_PEM" ]; then
  hex=$(sha256sum out/DDM.exe | cut -d' ' -f1)
  printf '%s' "ddm-host-v1|$hex" | openssl dgst -sha256 -sign "$DDM_LICENSE_KEY_PEM" -out out/DDM.exe.sig && echo "DDM.exe.sig written"
fi
ver=$(sed -n 's/#define DDM_VERSION "\(.*\)"/\1/p' src/common/version.hpp)
# the site rules every DDM picks up by itself (how sites show their videos): signed the same way
python3 -c "import json,sys; d=json.load(open('installer/sites.json')); sys.stdout.write(json.dumps(d, ensure_ascii=False, separators=(',',':')))" > out/sites.json
if [ -n "$DDM_LICENSE_KEY_PEM" ] && [ -f "$DDM_LICENSE_KEY_PEM" ]; then
  openssl dgst -sha256 -sign "$DDM_LICENSE_KEY_PEM" out/sites.json | base64 -w0 > out/sites.json.sig && echo "sites.json signed"
fi
(cd installer && makensis -V2 -DVER="$ver" installer.nsi)
sign_file "out/DDM-Setup-$ver.exe"
sh extension/build.sh
# the update information DDM reads from GitHub (and dedseec.com): signed with DedSec's key, with the setup's SHA-256
repo=$(sed -n 's/#define DDM_UPDATE_REPO "\(.*\)"/\1/p' src/common/version.hpp)
notes=${DDM_NOTES:-"Improvements and fixes."}
sha=$(sha256sum "out/DDM-Setup-$ver.exe" | cut -d' ' -f1)
python3 - "$ver" "$repo" "$sha" "$notes" > out/update.json <<'PY'
import json, sys
ver, repo, sha, notes = sys.argv[1:5]
print(json.dumps({"version": ver, "setup": f"https://github.com/{repo}/releases/download/v{ver}/DDM-Setup-{ver}.exe",
                  "sha256": sha, "url": f"https://github.com/{repo}/releases/latest", "notes": notes}, indent=2, ensure_ascii=False), end="")
PY
if [ -n "$DDM_LICENSE_KEY_PEM" ] && [ -f "$DDM_LICENSE_KEY_PEM" ]; then
  openssl dgst -sha256 -sign "$DDM_LICENSE_KEY_PEM" out/update.json | base64 -w0 > out/update.json.sig && echo "update.json signed"
else
  echo "update.json NOT signed (set DDM_LICENSE_KEY_PEM): DDM will ignore it"
fi
echo "done: out/DDM-Setup-$ver.exe  (GitHub release v$ver: upload DDM-Setup-$ver.exe, update.json, update.json.sig, sites.json, sites.json.sig)"
