#!/usr/bin/env bash
# Push firmware to the ESP32 over Wi-Fi and wait until the new image confirms itself.
#   ./ota.sh [host] [image]      defaults: esp32.local, build/esp32_status.bin
# Build first with: idf.py build
# The admin password (set on the device's setup/settings page) is read from
# $ESP32_ADMIN_PASSWORD, or prompted for.
set -euo pipefail
cd "$(dirname "$0")"

host=${1:-esp32.local}
image=${2:-build/esp32_status.bin}
pw=${ESP32_ADMIN_PASSWORD:-}
if [ -z "$pw" ]; then
    read -rsp "admin password for $host: " pw; echo
fi
[ -f "$image" ] || { echo "no image at $image (run idf.py build)" >&2; exit 1; }

field() { sed -n "s/.*\"$1\":\"\{0,1\}\([^\",}]*\).*/\1/p"; }

before=$(curl -fsS -m 10 "http://$host/api/status")
echo "current: $(field firmware <<<"$before") on $(field partition <<<"$before")"
echo "uploading $image ($(wc -c <"$image" | tr -d ' ') bytes) to $host ..."
# Header from stdin so the password doesn't show up in the process list.
resp=$(mktemp)
code=$(printf 'X-Admin-Password: %s\n' "$pw" | curl -sS -m 300 -H @- -H "Content-Type: application/octet-stream" \
       --data-binary @"$image" -w '%{http_code}' -o "$resp" "http://$host/api/ota" || true)
cat "$resp"; rm -f "$resp"
[ "$code" = 200 ] || { echo "upload rejected (HTTP $code)" >&2; exit 1; }

echo "waiting for reboot and confirmation (up to 6 min) ..."
sleep 10
said=
for _ in $(seq 72); do
    if s=$(curl -fsS -m 5 "http://$host/api/status" 2>/dev/null); then
        part=$(field partition <<<"$s") pending=$(field ota_pending <<<"$s")
        if [ "$part" = "$(field partition <<<"$before")" ]; then
            echo "FAILED: new firmware $(field rolled_back_from <<<"$s") was rolled back; still running $(field firmware <<<"$s") on $part" >&2
            exit 1
        fi
        if [ "$pending" = "false" ]; then
            echo "done: $(field firmware <<<"$s") on $part, confirmed"
            exit 0
        fi
        [ -n "$said" ] || echo "  up on $part, on probation (waiting for gateway check, rollback after 5 min)"
        said=1
    fi
    sleep 5
done
echo "timed out waiting for confirmation" >&2
exit 1
