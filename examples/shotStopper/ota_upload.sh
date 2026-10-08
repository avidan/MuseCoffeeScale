#!/usr/bin/env bash
# Upload a shotStopper build over WiFi.
#   ./ota_upload.sh path/to/shotStopper.ino.bin [host]
# Pauses the scale link first (POST /ota/prepare), since an active scale
# connection breaks OTA. Reads the password from ota_secret.h next to this file.
set -euo pipefail

BIN=${1:?usage: ota_upload.sh shotStopper.ino.bin [host]}
HOST=${2:-shotstopper-grinder.local}
DIR=$(cd "$(dirname "$0")" && pwd)

ESPOTA=$(ls ~/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/espota.py \
         ~/.arduino15/packages/esp32/hardware/esp32/*/tools/espota.py 2>/dev/null | tail -1 || true)
[ -n "$ESPOTA" ] || { echo "espota.py not found; install the esp32 Arduino core" >&2; exit 1; }

PW=""
if [ -f "$DIR/ota_secret.h" ]; then
  PW=$(sed -nE 's/.*MUSE_OTA_PASSWORD[[:space:]]+"([^"]*)".*/\1/p' "$DIR/ota_secret.h")
fi

# Resolve once so espota and curl hit the same board.
IP=$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$HOST")

STATUS=$(curl -fsS -m 5 "http://$IP/status")
case "$STATUS" in
  *'"brewing":true'*) echo "shot in progress, not updating" >&2; exit 1 ;;
esac

echo "Pausing scale link on $IP"
curl -fsS -m 5 -X POST "http://$IP/ota/prepare"; echo
sleep 2

echo "Uploading $BIN"
python3 "$ESPOTA" -i "$IP" -p 3232 -t 30 ${PW:+-a "$PW"} -f "$BIN"
