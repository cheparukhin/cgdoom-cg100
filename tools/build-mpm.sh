#!/usr/bin/env bash
# Builds mpm.bin (MPM's add-in menu and loader, the file at the root of the
# calculator's storage) from a pinned upstream commit into build-mpm/mpm.bin.
# MPM publishes no binary releases; 1.1 is the build that leaves CGDoom the most
# RAM (README, "Which mpm.bin"). This does not install the MPM OS patch.
#
#   tools/build-mpm.sh        # needs ./toolchain from tools/build-toolchain.sh
#
# MPM's build runs `python` (with PyYAML and Pillow) and GNU `stat -c`; this
# script provides both inside build-mpm/ without touching the system.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MPM_REPO=https://git.planet-casio.com/PlaneteCasio/mpm.git
MPM_COMMIT=cbd1a414d96b55e14adc20a88e2140f2c8723b5a  # "bump minor version to 1.1"
SRC="$ROOT/ref/mpm"
OUT="$ROOT/build-mpm"

export PATH="$ROOT/toolchain/bin:$PATH"
command -v fxsdk >/dev/null || { echo "build-mpm: no fxsdk in toolchain/bin; run tools/build-toolchain.sh" >&2; exit 1; }

[[ -d "$SRC/.git" ]] || git clone -q "$MPM_REPO" "$SRC"
git -C "$SRC" cat-file -e "$MPM_COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin

# Build from an export of the commit, so ref/mpm stays untouched.
rm -rf "$OUT"
mkdir -p "$OUT/src" "$OUT/bin"
git -C "$SRC" archive "$MPM_COMMIT" | tar -x -C "$OUT/src"

python3 -m venv --system-site-packages "$OUT/venv"
"$OUT/venv/bin/python" -c 'import yaml' 2>/dev/null || "$OUT/venv/bin/pip" install -q pyyaml
"$OUT/venv/bin/python" -c 'import PIL' 2>/dev/null || "$OUT/venv/bin/pip" install -q pillow

if ! stat -c %s "$0" >/dev/null 2>&1; then  # BSD stat (macOS)
  if command -v gstat >/dev/null; then
    ln -s "$(command -v gstat)" "$OUT/bin/stat"
  else
    cat >"$OUT/bin/stat" <<'EOF'
#!/bin/sh
if [ "$1" = "-c" ] && [ "$2" = "%s" ]; then shift 2; exec /usr/bin/stat -f %z "$@"; fi
exec /usr/bin/stat "$@"
EOF
    chmod +x "$OUT/bin/stat"
  fi
fi

(cd "$OUT/src" && PATH="$OUT/bin:$OUT/venv/bin:$PATH" fxsdk build-cg)
cp "$OUT/src/build-cg/mpm.bin" "$OUT/mpm.bin"
echo "build-mpm: $OUT/mpm.bin ($(wc -c <"$OUT/mpm.bin" | tr -d ' ') bytes)"
shasum -a 256 "$OUT/mpm.bin" 2>/dev/null || sha256sum "$OUT/mpm.bin"
