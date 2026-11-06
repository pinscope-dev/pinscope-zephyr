# Pinscope for Zephyr

Remote access, logs, metrics, fatal error reports and MCUboot firmware updates
for Zephyr devices, over one outbound connection to [Pinscope](https://pinscope.dev).
About 8.6 KB of RAM, no TLS library. Works behind NAT and on cellular, with no
open ports.

## Try it in five minutes

You need [west and the Zephyr SDK](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
and the project key from **Connect a device** in the [dashboard](https://pinscope.dev/#/)
(three devices are free).

```sh
west init -m https://github.com/pinscope-dev/pinscope-zephyr pinscope-ws
cd pinscope-ws && west update
west packages pip --install
export PINSCOPE_KEY=psk_...

west build -b nucleo_f429zi --sysbuild modules/lib/pinscope/sample
west flash
```

The board appears in the dashboard with its own HTTPS address. Open it, watch
its log, and open `/crash` on its page to see a fatal error reported after the
reboot. For an update, upload `build/sample/zephyr/zephyr.signed.bin` under
Firmware.

### No board? Run it in QEMU

```sh
west build -b mps2/an385 modules/lib/pinscope/sample
qemu-system-arm -M mps2-an385 -nographic -nic user,model=lan9118 -kernel build/zephyr/zephyr.bin
```

With QEMU from your system (`brew install qemu`, `apt install qemu-system-arm`),
which has the user networking this needs. The emulated board has no random
generator, so this build uses a predictable one: for trying, never for a device.

### Boards

The sample is set up for these; any board with a network works the same way
(`sample/boards` shows what a board needs). Each one is built on every push.

| Board | `-b` | Network | Before building |
|---|---|---|---|
| ST NUCLEO-F429ZI | `nucleo_f429zi` | Ethernet | |
| ST NUCLEO-H563ZI | `nucleo_h563zi` | Ethernet | |
| ST B-L475E-IOT01A | `disco_l475_iot1` | WiFi (ISM43362) | `WIFI_SSID`, `WIFI_PASSWORD` |
| Nordic nRF7002 DK | `nrf7002dk/nrf5340/cpuapp` | WiFi | `west blobs fetch nrf_wifi`, WiFi |
| Nordic nRF5340 DK + W5500 | `nrf5340dk/nrf5340/cpuapp` | Ethernet | add `-- -Dsample_SHIELD=wiznet_w5500` |
| Nordic nRF9151 DK | `nrf9151dk/nrf9151/ns` | LTE-M, NB-IoT | the nRF Connect SDK workspace below |
| Espressif ESP32-S3-DevKitC | `esp32s3_devkitc/esp32s3/procpu` | WiFi | `west blobs fetch hal_espressif`, WiFi |
| Espressif ESP32-C3-DevKitM | `esp32c3_devkitm` | WiFi | `west blobs fetch hal_espressif`, WiFi |
| Espressif ESP32-C6-DevKitC | `esp32c6_devkitc/esp32c6/hpcore` | WiFi | `west blobs fetch hal_espressif`, WiFi |
| NXP FRDM-RW612 | `frdm_rw612` | Ethernet | |
| Raspberry Pi Pico 2 W | `rpi_pico2/rp2350a/m33_0/w/mcuboot` | WiFi | `west blobs fetch hal_infineon`, WiFi |
| WIZnet W55RP20-EVB-Pico | `w55rp20_pinscope` | Ethernet | load `build/pinscope.uf2` with `picotool load -v -x` |
| QEMU (Arm MPS2) | `mps2/an385` | Ethernet | no `--sysbuild`; see above |

WiFi boards take the network from the environment when the build directory is
created: `WIFI_SSID=... WIFI_PASSWORD=... west build ...`.

On the nRF9151 and the B-L475E-IOT01A the modem runs the TCP/IP stack itself
(offloaded sockets). There the device's logs, metrics, crash reports and
updates work as everywhere, but its own web page cannot be reached through
Pinscope: that needs Zephyr's native IP stack.

### nRF91: the nRF Connect SDK

The nRF91 modem library is part of Nordic's nRF Connect SDK, not of upstream
Zephyr. A workspace with nRF Connect SDK 3.4.2:

```sh
west init -m https://github.com/pinscope-dev/pinscope-zephyr --mf west-ncs.yml pinscope-ncs
cd pinscope-ncs && west update
pip install -r nrf/scripts/requirements-build.txt
PINSCOPE_KEY=psk_... west build -b nrf9151dk/nrf9151/ns --sysbuild modules/lib/pinscope/sample
```

The board needs a SIM with an LTE-M or NB-IoT plan; LTE comes up at boot.

## Use it in your application

In your `west.yml`, under `projects`, then `west update`:

```yaml
- name: pinscope
  url: https://github.com/pinscope-dev/pinscope-zephyr
  revision: v0.2.2
  path: modules/lib/pinscope
```

In `prj.conf`:

```
CONFIG_PINSCOPE=y
CONFIG_PINSCOPE_PROJECT_KEY="psk_..."
```

No code: the module turns on what it needs and starts at boot. Messages from
`LOG_*` go to the dashboard, fatal errors are kept across the reboot, and the
build says if a setting is missing. For updates, build with sysbuild and
MCUboot (`SB_CONFIG_BOOTLOADER_MCUBOOT=y`), and sign with your own key
(`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`), not MCUboot's published development key.

Documentation: https://pinscope.dev/docs/zephyr

## Licence

Source-available: the licence in `LICENSE` allows use in any product, including
commercial ones, for connecting to [Pinscope](https://pinscope.dev). Questions:
hello@pinscope.dev.
