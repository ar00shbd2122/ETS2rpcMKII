#!/usr/bin/env bash
#
# download_assets.sh - fetches the official ETS2 icon + the 7 licensed
# ETS2 brand logos into assets/portal/, ready to upload in the Discord
# Developer Portal (your app -> Rich Presence -> Art Assets).
#
# The small badge in a presence is the truck brand logo. Brand SVGs
# are rasterized server-side by Wikimedia's Special:FilePath renderer,
# so they arrive as transparent PNGs - no local image tooling needed.
#
# Upload rule: the asset NAME in the portal is the file name without
# the .png extension (ets2.png uploads as ets2). The name field only
# accepts letters, numbers, underscores and hyphens - no slashes, no
# dots. "String value did not match validation regex" means a slash
# or dot is still in the field.
#
# Usage:  bash tools/download_assets.sh
#
set -u

UA="ETS2rpcMKII-asset-fetcher/1.0"
OUT="$(cd "$(dirname "$0")/.." && pwd)/assets/portal"
mkdir -p "$OUT"

# name|url  (asset key in code MUST match `name`, lowercase, underscores ok)
ASSETS=(
  # official round ETS2 app icon (the big presence image)
  "ets2|https://cdn.discordapp.com/app-icons/1402418075726254102/46bfdfc58e4eacbfde29b2461e118e78.png?size=1024&keep_aspect_ratio=false"
  # the 7 officially licensed ETS2 brands, transparent PNG straight
  # from the renderer. ATS brands (ford, mack, kenworth, peterbilt)
  # are intentionally not fetched: they do not belong in ETS2.
  "scania|https://en.wikipedia.org/wiki/Special:FilePath/Scania_Logo.svg?width=1024"
  "volvo|https://commons.wikimedia.org/wiki/Special:FilePath/Volvo_logo.svg?width=1024"
  "daf|https://commons.wikimedia.org/wiki/Special:FilePath/DAF_logo.svg?width=1024"
  "man|https://commons.wikimedia.org/wiki/Special:FilePath/MAN_logo.svg?width=1024"
  "mercedes|https://commons.wikimedia.org/wiki/Special:FilePath/Mercedes-Logo.svg?width=1024"
  "renault|https://commons.wikimedia.org/wiki/Special:FilePath/Renault_logo.svg?width=1024"
  "iveco|https://commons.wikimedia.org/wiki/Special:FilePath/IVECO_logo.jpg?width=1024"
  # generic lorry pictogram for unknown and modded brands
  "generic|https://commons.wikimedia.org/wiki/Special:FilePath/Sinnbild_LKW.svg?width=1024"
)

fail=0
for entry in "${ASSETS[@]}"; do
  name="${entry%%|*}"
  url="${entry#*|}"
  dst="$OUT/$name.png"
  tmp="$(mktemp)"

  code=$(curl -sL -A "$UA" -o "$tmp" -w "%{http_code}" --retry 3 --retry-delay 2 "$url")
  ctype=$(file -b --mime-type "$tmp" 2>/dev/null || true)

  if [ "$code" = "200" ] && { [ "$ctype" = "image/png" ] || [ "$ctype" = "image/jpeg" ]; }; then
    mv "$tmp" "$dst"
    echo "OK  $name.png"
  else
    echo "FAIL $name (HTTP $code, got $ctype)"
    rm -f "$tmp"
    fail=1
  fi
  sleep 1   # be polite to wikimedia
done

# Cleanup pass (needs python + Pillow; skipped gracefully otherwise):
# 1. logos without alpha get their near-white backdrop removed
# 2. wide logos are centered on a square transparent canvas, because
#    the Developer Portal rejects anything smaller than 512x512
# 3. anything above 1024x1024 is scaled down, the portal's maximum
if command -v python >/dev/null 2>&1 && python -c "import PIL" >/dev/null 2>&1; then
  python - "$OUT" <<'PY'
import os, sys
from PIL import Image
out = sys.argv[1]
for fn in sorted(os.listdir(out)):
    if not fn.endswith(".png"):
        continue
    p = os.path.join(out, fn)
    im = Image.open(p).convert("RGBA")

    # 1. background strip for fully opaque images
    lo, _ = im.getchannel("A").getextrema()
    if lo == 255:
        px = list(im.getdata())
        hit = False
        for i, (r, g, b, a) in enumerate(px):
            if r > 235 and g > 235 and b > 235:
                px[i] = (255, 255, 255, 0); hit = True
            elif r > 200 and g > 200 and b > 200:
                px[i] = (r, g, b, int(255 * (235 - min(r, g, b)) / 35)); hit = True
        if hit:
            im.putdata(px)
            print("bg-strip", fn)

    # 2. pad to square, minimum 512 (portal minimum)
    w, h = im.size
    side = max(w, h, 512)
    if (w, h) != (side, side):
        canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        canvas.paste(im, ((side - w) // 2, (side - h) // 2), im)
        im = canvas
        print("square  ", fn)

    # 3. cap at 1024 (portal maximum)
    if im.size[0] > 1024:
        im = im.resize((1024, 1024), Image.LANCZOS)
        print("resized ", fn)

    im.save(p)
PY
fi

echo
echo "Done. Upload everything in $OUT to:"
echo "  Discord Developer Portal -> your app -> Rich Presence -> Art Assets"
echo "Asset NAME = file name without .png (ets2.png -> ets2)."
echo "Lowercase, no slashes or dots in the name field."
exit $fail
