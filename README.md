# Pinscope for Zephyr

Remote access, logs, metrics, fatal error reports and MCUboot firmware updates
for Zephyr devices, over one outbound connection to [Pinscope](https://pinscope.dev).
About 8.6 KB of RAM, no TLS library. Works behind NAT and on cellular, with no
open ports.

## Try it in five minutes

You need [west and the Zephyr SDK](https://docs.zephyrproject.org/latest/develop/getting_started/index.html),
a board with a network, and a free Pinscope account: the project key is under
**Connect a device** in the [dashboard](https://pinscope.dev/#/).

```sh
west init -m https://github.com/pinscope-dev/pinscope-zephyr pinscope-ws
cd pinscope-ws && west update
west packages pip --install

export PINSCOPE_KEY=psk_...
# Ethernet, with MCUboot for updates:
west build -b nucleo_f429zi --sysbuild modules/lib/pinscope/sample
# WiFi (the nRF7002's firmware comes as a blob, once):
west blobs fetch nrf_wifi
WIFI_SSID=... WIFI_PASSWORD=... west build -b nrf7002dk/nrf5340/cpuapp --sysbuild modules/lib/pinscope/sample
# nRF5340 DK with a WIZnet W5500 Ethernet shield:
west build -b nrf5340dk/nrf5340/cpuapp --sysbuild modules/lib/pinscope/sample -- -Dsample_SHIELD=wiznet_w5500
west flash
```

The board appears in the dashboard with its own HTTPS address. Open it, watch
its log, and try `/crash` on its page to see a fatal error reported after the
reboot. For an update, upload `build/sample/zephyr/zephyr.signed.bin` under
Firmware.

Boards the sample is set up for: `nrf7002dk/nrf5340/cpuapp` (WiFi),
`nrf5340dk/nrf5340/cpuapp` (Ethernet over a W5500 shield),
`nucleo_f429zi` (Ethernet) and `w55rp20_pinscope` (WIZnet W55RP20, Ethernet).
Any board with a network works; see `sample/boards` for what a board needs.

## Use it in your application

In your `west.yml`, under `projects`, then `west update`:

```yaml
- name: pinscope
  url: https://github.com/pinscope-dev/pinscope-zephyr
  revision: v0.1.0
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
