#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(awk -F= '/^version=/{print $2; exit}' "$ROOT_DIR/library.properties")
PACKAGE_NAME="BitChat_ESP32"
OUT_DIR="$ROOT_DIR/dist"
OUT_ZIP="$OUT_DIR/$PACKAGE_NAME-$VERSION.zip"
TMP_PARENT="${TMPDIR:-/tmp}/bitchat_esp32_release_$$"
TMP_DIR="$TMP_PARENT/$PACKAGE_NAME"

if [ -z "$VERSION" ]; then
  echo "version= not found in library.properties" >&2
  exit 1
fi

if ! command -v zip >/dev/null 2>&1; then
  echo "zip command not found" >&2
  exit 1
fi

cleanup() {
  rm -rf "$TMP_PARENT"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP_DIR" "$OUT_DIR"

for path in library.properties keywords.txt LICENSE NOTICE.md README.md src examples docs; do
  if [ -e "$ROOT_DIR/$path" ]; then
    cp -R "$ROOT_DIR/$path" "$TMP_DIR/"
  fi
done

find "$TMP_DIR" -name ".DS_Store" -delete
find "$TMP_DIR" -name "__MACOSX" -type d -prune -exec rm -rf {} +

rm -f "$OUT_ZIP"
(cd "$TMP_PARENT" && zip -qr "$OUT_ZIP" "$PACKAGE_NAME")

echo "Created $OUT_ZIP"
echo "Arduino IDE: Sketch > Include Library > Add .ZIP Library..."
