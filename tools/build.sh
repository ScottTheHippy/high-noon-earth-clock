#!/bin/sh
# Bake the Earth texture, compile, and (optionally) upload.
# usage: tools/build.sh [serial-port]
set -e
cd "$(dirname "$0")/.."
CLI="${ARDUINO_CLI:-arduino-cli}"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M"
TEXTURE_URL="https://eoimages.gsfc.nasa.gov/images/imagerecords/57000/57752/land_shallow_topo_2048.jpg"

[ -f tools/earth_day.jpg ] || curl -fL -o tools/earth_day.jpg "$TEXTURE_URL"
python3 tools/bake_tex.py tools/earth_day.jpg .

if [ -n "$1" ]; then
  "$CLI" compile --upload -p "$1" --fqbn "$FQBN" --build-property upload.maximum_size=8388608 .
  sleep 6
  printf 'T%s\n' "$(date +%s)" > "$1"
else
  "$CLI" compile --fqbn "$FQBN" --build-property upload.maximum_size=8388608 .
fi
