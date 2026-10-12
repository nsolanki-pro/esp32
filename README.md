# ESP32 low-power Wi-Fi node

ESP-IDF firmware for a generic ESP32 dev board (ESP32-D0WD, CP2102 USB-serial). It joins your
Wi-Fi, serves a small status web UI, keeps itself connected, sleeps whenever it can, and updates
over the air with automatic rollback. No credentials are compiled in: Wi-Fi and the admin password
are set on the device.

- **Wi-Fi setup over a WPA3 access point** with a captive portal: no reflashing to change networks
- **Low power**: automatic light sleep + Wi-Fi modem sleep (the CPU sleeps ~90-95% of the time when idle)
- **Self-healing**: reconnects with backoff; pings the gateway every 5 min and escalates
  (reconnect → restart Wi-Fi → reboot) if it stops answering
- **OTA updates** from the browser or a script; a new image must prove it can reach the network or
  the bootloader rolls back to the previous one
- **Factory reset** from the web UI or by holding BOOT for 20 s
- **Web settings**: hostname, time zone (NTP clock), LED mode, static IP, Wi-Fi TX power and power
  save, reboot, switch between the two firmware slots, live log viewer; network changes are tested
  and undone automatically if the device loses the network
- Optional **CSI motion sensor** build (Wi-Fi channel state information, no extra hardware)

## Quick start (prebuilt release)

1. Download `esp32-<version>-full.bin` from the [latest release](../../releases/latest).
2. Flash it over USB (any ESP32 board, erasing first is recommended for a new board):

   ```sh
   pip install esptool
   esptool --chip esp32 -b 115200 erase-flash
   esptool --chip esp32 -b 115200 write-flash 0x0 esp32-<version>-full.bin
   ```

   Use 115200 baud; some cheap boards are unreliable at higher rates.
3. Set up Wi-Fi (next section).

## Wi-Fi setup

On first boot (or after a factory reset) the board opens a setup network:

| | |
|---|---|
| Network | `ESP32-Setup-XXXX` (XXXX = last 4 hex digits of the MAC) |
| Password | `esp32setupXXXX` (same XXXX, same case) |
| Security | WPA3-SAE: someone nearby can't read the setup form even if they know the password |

1. Join it from a phone or laptop. A sign-in page should pop up; if not, open `http://192.168.4.1/setup`.
2. Tap **Scan for networks**, pick your network (2.4 GHz only) and enter its password.
3. Choose an **admin password** (8+ characters). It protects updates, Wi-Fi changes and reset.
4. **Connect and save.** The board tests the credentials first: a typo gives "wrong password"
   instead of a dead device. On success it shows its new IP address and reboots.
5. Switch back to your network and open `http://<ip>/` or `http://esp32.local/` (or the hostname
   you set later in *Settings*).

If the saved network is unreachable for 2 minutes (router off, password changed), the setup network
comes back alongside normal reconnect attempts. Changing Wi-Fi from there needs the admin password.

## Web UI

| Page | |
|---|---|
| `/` | Status: network, gateway health, time, uptime, last reset reason, memory, firmware, chip/flash/MAC |
| `/logs` | Live log: the last 8 KB of the device log, no USB cable needed (`/api/logs?from=N`) |
| `/update` | Install the latest release or switch build variant, boot the other app slot, upload a file |
| `/settings` | Device and network options, admin password, reboot, factory reset |
| `/setup` | Change Wi-Fi network (admin password once set) |
| `/api/power` | Time spent active / idle / in light sleep |
| `/api/wakes` | Received frames by kind: what keeps waking the radio (`?reset=1` clears) |

Everything that changes the device needs the admin password (`X-Admin-Password` header in the API).
A status banner appears when a newer release is known.

## Settings

**Device** (saved at once):

| Option | |
|---|---|
| Hostname | mDNS name (`<name>.local`, immediately) and DHCP host name (router sees it after the next reconnect). Default `esp32` |
| Time zone | POSIX TZ string (presets for common zones). The clock syncs from `pool.ntp.org` after connecting and every 6 h; log lines then carry local time |
| LED | *Activity* (default: blink while connecting, flash per request), *On while connected*, or *Off*. The BOOT-button countdown always uses the LED |
| Gateway check | Health-check interval, 30-3600 s (default 300) |
| Daily update check | Off by default. Checks GitHub once a day and shows a banner; it never installs by itself |

**Network** (*Apply and test*): DHCP or static IP (IP, netmask, gateway, DNS; an empty DNS keeps the
server in use now), Wi-Fi TX power (2-20 dBm; the chip rounds to its nearest power step), and the
power-save listen interval. Applying reconnects Wi-Fi and pings the gateway. If that doesn't work within
30 s, the previous settings are restored by themselves and the page says why. As a second safety net,
if the network later stays unreachable with a static IP or reduced TX power (Wi-Fi down 2 min, or the
gateway fails three checks), the device goes back to DHCP and full power on its own and logs it.

API: `GET/POST /api/config`, `GET/POST /api/net` (POST returns at once; poll GET for
`testing` → `ok` / `reverted`), `POST /api/reboot`.

**Reboot** is refused while new firmware is still on probation (about the first 30 s after an update),
because a reset then would roll it back.

## OTA updates

Updates replace only the app; Wi-Fi settings and the admin password are kept.

**Latest release, one click:** open `/update`. The board checks GitHub for the newest release and
shows *Update available: vX.Y.Z* with an **Install** button. Enter the admin password and press it:
the board downloads the image itself over HTTPS, checks its SHA-256 against the release's
`manifest.json` and the image format, and only then switches to it (about 15-20 s, then the usual
probation and rollback). Nothing is installed or even checked unless you open the page or ask:
there is no background polling. Right after a release is published, GitHub may keep reporting the
previous one as "latest" for a few minutes.

The repository is a build option (`idf.py menuconfig` → *App* → *GitHub repository for firmware
updates*, empty disables it). API: `GET /api/update`, `POST /api/update/check`,
`POST /api/update/install` (admin password).

**Switching build variant:** every release has a default (low power) and a motion (CSI sensor) image,
listed in its `manifest.json`. `/update` offers *Switch to the motion build* (or back); the switch
works at any version, so it also lets you go to the other variant of the same release. API:
`POST /api/update/install?variant=motion`.

**Other app slot:** the board keeps the previous firmware in its second slot. `/update` lists both
slots (version, build date, state) and *Boot …* switches to the other one. The switched-to image runs on
probation like an update, so if it can't reach the network the board comes back by itself. API:
`GET /api/slots`, `POST /api/slots/boot` (form `slot=ota_0`).

**Upload a file from the browser:** open `/update`, choose `esp32-<version>-ota.bin` (from a release, or
`firmware/build/esp32_status.bin` from your own build), enter the admin password, **Upload**. The page
follows the reboot and reports *confirmed* or *rolled back*.

**From a shell:**

```sh
cd firmware
./ota.sh esp32.local                         # uploads build/esp32_status.bin
./ota.sh 192.168.1.50 esp32-v1.0.0-ota.bin   # or any image, by IP
```

The admin password is read from `$ESP32_ADMIN_PASSWORD` or prompted for.

**Rollback:** the board has two app slots. A new image boots "on probation" and is confirmed by its
first successful gateway ping (about 30 s after boot). If it isn't confirmed within 5 minutes, crashes
or resets first, or the health check decides to reboot, the bootloader returns to the previous image.
`/api/status` shows `firmware`, `partition`, `ota_pending` and `rolled_back_from`.

Uploads are checked before anything is written: wrong chip, wrong project, merged/full images,
truncated or corrupt files are rejected.

## Factory reset

- **Web:** `/settings` → *Reset to setup mode* (admin password), or
- **Button:** hold **BOOT** for 20 s while the board is running. The LED blinks (faster in the last
  5 s), then stays on; releasing early cancels.

Both erase Wi-Fi settings, the admin password and all options and reboot into setup mode. Firmware is kept.
Holding BOOT *while* resetting or powering up enters the ROM download mode instead; that's normal.

Forgot the admin password? Use the BOOT button, or erase the flash over USB and reflash.

## Building

Requires [ESP-IDF v6.0.3](https://docs.espressif.com/projects/esp-idf/en/v6.0.3/esp32/get-started/).

```sh
cd firmware
idf.py build                                  # low-power build
idf.py -p /dev/ttyUSB0 -b 115200 flash        # first install over USB
```

The first USB flash installs the bootloader and the two-slot partition table; after that, use OTA.

**CSI motion sensor variant** (radio always on, no light sleep; adds `/motion` and `/api/motion`):

```sh
idf.py -B build-motion -D SDKCONFIG=build-motion/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.motion" build
```

**Options** (`idf.py menuconfig` → *App*): Wi-Fi listen interval, health-check interval, how long
Wi-Fi may stay down before a reboot, CSI motion build.

## Releases

Pushing a tag builds both variants on GitHub Actions and publishes a release:

```sh
git tag v1.1.0 && git push origin v1.1.0
```

| Asset | Use |
|---|---|
| `esp32-<version>-ota.bin` | OTA update (`/update` or `ota.sh`) |
| `esp32-<version>-full.bin` | USB flash at offset `0x0` (new or erased board) |
| `esp32-<version>-motion-*` | Same, CSI motion variant |
| `SHA256SUMS` | Checksums |
| `manifest.json` | Version, file names and SHA-256 that the device reads for one-click updates |

The version shown by the device comes from `git describe`, so a tagged build reports `v1.1.0`.
The workflow can also be run manually (*Actions → Build firmware → Run workflow*); the binaries are
then attached to the run instead of a release.

## Security notes

- The web UI is plain HTTP. On your LAN the admin password travels unencrypted; your Wi-Fi's own
  encryption protects it from outsiders, not from other devices on the same network.
- The setup network uses WPA3 with a password derived from the MAC, which is visible in the network
  name. Its purpose is encryption, not keeping people out: while a new or reset board is in setup
  mode, whoever configures it first owns it.
- Firmware images are validated (format, chip, project, checksums) but not cryptographically signed.
  Anyone with the admin password can install any image built for this project.
- One-click updates trust the GitHub repository: TLS proves the files come from GitHub and the
  SHA-256 proves they are intact, but anyone who can publish a release there can ship firmware to
  devices that press *Install*.

## License

GPL-3.0, see [LICENSE](LICENSE).
