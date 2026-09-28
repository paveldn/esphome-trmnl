# ESPHome TRMNL component

[![CI](https://github.com/paveldn/esphome-trmnl/actions/workflows/ci.yml/badge.svg)](https://github.com/paveldn/esphome-trmnl/actions/workflows/ci.yml)

This external component lets an ESPHome device fetch screens from the
[TRMNL](https://usetrmnl.com) BYOD API. It handles the protocol and leaves the
hardware-specific work to ESPHome: `online_image` downloads the image, your
display renders it, and `deep_sleep` controls the wake cycle.

The component works with the TRMNL cloud and with compatible self-hosted
servers. It is not an official TRMNL or ESPHome project.

## What it does

- Calls `/api/setup` when no API key is configured, then stores the returned
  key and friendly ID in flash.
- Calls `/api/display` with the standard TRMNL headers, including optional
  battery and display information.
- Reports successful responses through `on_image_available` and failures
  through `on_error`.
- Remembers the last image URL and refresh rate across deep sleep.
- Supports the protocol's one-shot special-function request.

It does not download images, drive a display, or put the device to sleep. Those
jobs remain in the normal ESPHome configuration.

## Installation

```yaml
external_components:
  - source: github://paveldn/esphome-trmnl@main
    components: [trmnl]
```

For local development, replace the source with a path to this repository's
`components` directory.

## Basic configuration

```yaml
http_request:

trmnl:
  id: trmnl_client
  base_url: https://trmnl.com
  api_key: !secret trmnl_api_key
  width: 800
  height: 480
  on_image_available:
    - if:
        condition:
          lambda: return !image_url.empty() && image_changed;
        then:
          - online_image.set_url:
              id: trmnl_image
              url: !lambda return image_url;
  on_error:
    - logger.log:
        format: "TRMNL error: %s"
        args: [error_type.c_str()]
```

Omit `api_key` to use `/api/setup`. After registration, the friendly ID is
shown in the ESPHome logs. It is also available in lambdas as
`id(trmnl_client).get_friendly_id()`.

The component does not poll by itself. Call `trmnl.update` from `on_boot`, an
interval, a button, or another automation:

```yaml
esphome:
  on_boot:
    priority: -100
    then:
      - trmnl.update: trmnl_client
```

See [the ESP32 example](examples/esp32-example.yaml) for a complete display and
deep-sleep setup. [The host example](examples/host-example.yaml) is useful for
testing the protocol without e-paper hardware.

## Drawing the image

`on_image_available` runs after the JSON response has been parsed, not after
the image has been downloaded. Trigger the display update from
`online_image.on_download_finished`:

```yaml
image:
  - platform: online_image
    id: trmnl_image
    url: https://example.invalid/placeholder.bmp
    format: BMP
    type: BINARY
    resize: 800x480
    update_interval: never
    on_download_finished:
      - component.update: eink_display

display:
  - platform: waveshare_epaper
    id: eink_display
    # Configure the model and pins for your board.
    update_interval: never
    lambda: |-
      it.image(0, 0, id(trmnl_image));
```

For a deep-sleeping device, enter sleep from `on_download_finished` (and from
`online_image.on_error`), after the display update. Calling
`deep_sleep.enter` directly from `on_image_available` can put the device to
sleep before the asynchronous download completes.

`image_changed` is based on the URL, not the downloaded bytes. It is useful for
servers that issue a new URL for each screen. If your server reuses one URL for
changing content, ignore `image_changed` and download every response.

## Special functions

The TRMNL dashboard decides which special function is active for a device. The
protocol only lets firmware ask the server to run that configured function:

```yaml
on_...:
  - trmnl.trigger_special_function: trmnl_client
  - trmnl.update: trmnl_client
```

The first action adds `special_function: true` to the next display request. The
result arrives through the usual `on_image_available` or `on_error` callback.

Functions handled entirely by the server, such as identify, sleep, restart
playlist, guest mode, and refresh, fit this model. Rewind and send-to-me depend
on the official firmware's image cache, which this component does not provide.
The add-WiFi function also has no direct ESPHome equivalent.

## Platform support

The protocol component supports the platforms provided by ESPHome's
`http_request` component: ESP32 with Arduino or ESP-IDF, ESP8266, RP2040, and
host. RP2040 and ESP8266 require `verify_ssl: false` for HTTPS because ESPHome
does not support certificate verification on those platforms.

The usual image workflow has a narrower range. `online_image` supports ESP32,
RP2040, and host, but not ESP8266. An ESP8266 build can therefore use the TRMNL
protocol callbacks but cannot use the standard runtime image decoder.

## Reference

The full option and automation reference is in [docs/trmnl.mdx](docs/trmnl.mdx).

The main limitations are:

- Display responses are capped at 4 KiB. This is ample for the documented JSON
  payload but not for arbitrary server responses.
- Firmware-update and reset fields returned by the API are ignored. Firmware
  updates should go through ESPHome's OTA facilities instead.
- HTTP requests use ESPHome's synchronous `http_request` API, so an update can
  block until the configured HTTP timeout expires.

## Development and upstreaming

The source, tests, and documentation follow the layouts used by the ESPHome
projects:

| This repository | Upstream destination |
|---|---|
| `components/trmnl/` | `esphome/components/trmnl/` |
| `tests/components/trmnl/` | `tests/components/trmnl/` |
| `docs/trmnl.mdx` | `esphome-docs/src/content/docs/components/trmnl.mdx` |

The component tests are ready to copy into the ESPHome source tree and run with
its test harness. Component documentation is submitted separately to the
`esphome-docs` repository.

GitHub Actions runs the same tests against the current ESPHome `dev` branch.
Every supported architecture/framework combination is compiled in its own job,
so a failure on one platform does not hide results from the others.

In an ESPHome checkout, use the project's test and formatting commands:

```bash
script/test_build_components -e compile -c trmnl
script/ci-custom.py
```

## License

[MIT](LICENSE)
