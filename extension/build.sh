#!/bin/sh
# Builds out/extension/{chromium,firefox} and the three store packages (stores get no "key").
set -e
cd "$(dirname "$0")"
OUT=${1:-../out}
for b in chromium firefox; do rm -rf $OUT/extension/$b; mkdir -p $OUT/extension/$b; cp shared/* $OUT/extension/$b/; cp $b/manifest.json $OUT/extension/$b/; done
for st in chrome edge; do
  rm -rf $OUT/store-$st; cp -r $OUT/extension/chromium $OUT/store-$st
  python3 -c "import json,sys;p=sys.argv[1];m=json.load(open(p));m.pop('key',None);json.dump(m,open(p,'w'),indent=2,ensure_ascii=False)" $OUT/store-$st/manifest.json
  rm -f $OUT/ddm-$st-addon.zip; (cd $OUT/store-$st && zip -qr9 -X ../ddm-$st-addon.zip .); rm -rf $OUT/store-$st
done
rm -f $OUT/ddm-firefox-addon.zip; (cd $OUT/extension/firefox && zip -qr9 -X ../../ddm-firefox-addon.zip .)
# for trying it before the stores: the Chromium add-on with its fixed ID (load it with "Load unpacked")
rm -f $OUT/ddm-chromium-test-unpacked.zip; (cd $OUT/extension/chromium && zip -qr9 -X ../../ddm-chromium-test-unpacked.zip .)
