# Component build tests

The files under `tests/components/trmnl` exercise every configuration option,
both actions, and both automation callbacks. The platform files cover the
architecture/framework combinations required by ESPHome:

- ESP32 Xtensa with Arduino and ESP-IDF
- ESP32-C3 RISC-V with Arduino and ESP-IDF
- ESP8266 with Arduino
- RP2040 with Arduino
- host, as an additional protocol smoke build

Run the tests from an ESPHome checkout with:

```bash
script/test_build_components -e config -c trmnl
script/test_build_components -e compile -c trmnl
```

The repository's `CI` workflow runs both commands for every target listed
above against the current ESPHome `dev` branch.

These files rely on the platform base configurations supplied by ESPHome's test
runner. They are intended to be copied directly to `tests/components/trmnl`
in the ESPHome source tree.

The RP2040 and ESP8266 configurations disable TLS certificate verification
because ESPHome does not provide certificate verification on those platforms.
