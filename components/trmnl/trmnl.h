#pragma once

#include <string>
#include <vector>

#include "esphome/components/http_request/http_request.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::trmnl {

/// Persisted across reboots/deep-sleep cycles: the API key and friendly ID
/// obtained from `/api/setup` (when the device self-registers rather than
/// being given a key in config), the refresh_rate last received from
/// `/api/display` (reported back to the server on the next request, like the
/// official firmware does), and a hash of the last-seen image_url (to
/// compute `image_changed` without needing to keep the full URL around).
struct TrmnlStoredState {
  char api_key[64];
  char friendly_id[16];
  uint32_t refresh_rate;
  uint32_t image_url_hash;
};

/// Outcome of a /api/setup self-registration attempt.
enum class RegisterOutcome {
  REGISTERED,     ///< Got and stored an API key.
  NOT_LINKED,     ///< Request succeeded, but the server says this device isn't linked yet.
  NETWORK_ERROR,  ///< The request failed or returned a non-success HTTP status.
  PARSE_ERROR,    ///< The response was malformed or did not contain an API key.
};

/**
 * @brief Implements the TRMNL BYOD device protocol (GET /api/setup, /api/display).
 *
 * This component is intentionally a thin protocol/orchestration layer: it
 * builds the request, parses the JSON response, and fires exactly two
 * automation triggers -- `on_image_available` for anything that succeeded
 * (a normal update, the initial setup screen, or a special function result,
 * which may or may not carry an image), and `on_error` for anything that
 * didn't, with a string identifying which kind. It never touches sleep/power
 * (that's the job of ESPHome's own deep_sleep component, wired from whichever
 * of those two triggers fired) and never decodes images itself (that's the
 * job of ESPHome's `image: platform: online_image`, wired from
 * `on_image_available`).
 */
class TrmnlComponent final : public Component, public Parented<http_request::HttpRequestComponent> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_base_url(std::string base_url) { this->base_url_ = std::move(base_url); }
  /**
   * @brief Set a pre-provisioned API key (e.g. from the TRMNL dashboard).
   *
   * Optional: if left unset, the device will self-register via `/api/setup`
   * (identified only by its MAC address) the first time an update is
   * requested, and persist the key it's given for all future requests and
   * reboots. This matches how the official BYOD firmware behaves, and is
   * what most self-hosted BYOS servers with auto-provisioning expect. The
   * friendly_id a successful registration returns (needed to claim the
   * device on the TRMNL dashboard) is persisted and logged via
   * dump_config() -- there's no dedicated trigger for it, to keep this
   * component's automation surface to just two events. Read it from a
   * lambda with get_friendly_id() instead -- e.g. to show it on-screen, or
   * to publish it through a `text_sensor: platform: template` if you want
   * it visible in Home Assistant.
   */
  void set_api_key(std::string api_key) { this->api_key_ = std::move(api_key); }
  void set_model(std::string model) { this->model_ = std::move(model); }
  void set_width(int width) { this->width_ = width; }
  void set_height(int height) { this->height_ = height; }
  /// The code to claim this device on the TRMNL dashboard, from a successful
  /// self-registration -- persisted, so still valid after a reboot; empty if
  /// the device hasn't self-registered (e.g. `api_key` was set explicitly).
  const std::string &get_friendly_id() const { return this->friendly_id_; }
#ifdef USE_SENSOR
  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { this->voltage_sensor_ = voltage_sensor; }
  void set_battery_level_sensor(sensor::Sensor *battery_level_sensor) {
    this->battery_level_sensor_ = battery_level_sensor;
  }
#endif
#ifdef USE_BINARY_SENSOR
  void set_charging_binary_sensor(binary_sensor::BinarySensor *charging_binary_sensor) {
    this->charging_binary_sensor_ = charging_binary_sensor;
  }
#endif

  /**
   * @brief Poll the TRMNL `/api/display` endpoint once.
   *
   * This does not schedule itself again and does not sleep. Call it from your
   * own automations (e.g. `on_boot`, a `script`, or right before entering
   * deep sleep) -- polling cadence and power management are left entirely to
   * the rest of your ESPHome configuration.
   */
  void check_for_update();

  /**
   * @brief Request a TRMNL "special function" on the next `check_for_update()`.
   *
   * Sends `special_function: true` on that one request (matching the official
   * firmware exactly), consumed whether or not the request succeeds. Which
   * function actually runs is decided entirely server-side -- by whatever is
   * configured in that device's Controls settings on the TRMNL dashboard, per
   * https://help.trmnl.com/en/articles/9672080-special-functions. There is no
   * way to request a *specific* function; the protocol doesn't support that.
   * The device doesn't need to know which one ran either -- the result comes
   * back through the same `on_image_available`/`on_error` triggers as any
   * other update, since "here's an image and/or a refresh_rate to act on" is
   * all a special function result ever is from the device's side.
   */
  void trigger_special_function() { this->special_function_requested_ = true; }

  template<typename F> void add_on_image_available_callback(F &&callback) {
    this->image_available_callback_.add(std::forward<F>(callback));
  }
  template<typename F> void add_on_error_callback(F &&callback) {
    this->error_callback_.add(std::forward<F>(callback));
  }

 protected:
  std::vector<http_request::Header> build_headers_(bool for_setup);
  void handle_response_(const uint8_t *data, size_t len);
  /// Self-register via `/api/setup` when no API key is configured yet.
  RegisterOutcome register_device_();
  void load_state_();
  void save_state_();
  /// Compares `image_url` against the last one seen, updating and persisting
  /// state if it changed. Returns false (no state change) for an empty URL.
  bool update_image_hash_(const std::string &image_url, bool &needs_save);

  std::string base_url_;
  std::string api_key_;
  std::string model_{"esphome"};
  /// friendly_id returned by a successful /api/setup registration -- the
  /// human-readable code you claim the device with on the TRMNL dashboard.
  /// Persisted and only ever surfaced via dump_config() -- see set_api_key().
  std::string friendly_id_;
  int width_{0};
  int height_{0};
  /// Last refresh_rate received from the server; reported back on the next
  /// request via the `Refresh-Rate` header, and persisted so it survives
  /// deep sleep/reboots.
  uint32_t last_refresh_rate_{0};
  /// fnv1_hash() of the last image_url reported via on_image_available, used
  /// to compute `image_changed`. Persisted; 0 means "no image seen yet" (an
  /// astronomically unlikely hash for a real URL, and low-stakes either way
  /// since this only ever affects an optimization hint, not correctness).
  uint32_t last_image_url_hash_{0};
  /// Set by trigger_special_function(); consumed (reset to false) by the
  /// next check_for_update() regardless of outcome.
  bool special_function_requested_{false};

  ESPPreferenceObject pref_;

#ifdef USE_SENSOR
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *battery_level_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *charging_binary_sensor_{nullptr};
#endif

  // (image_url, filename, refresh_rate, image_changed) -- image_url and
  // filename may be empty (e.g. a special function that only changes
  // refresh_rate, like "sleep"); always check image_url before drawing.
  CallbackManager<void(std::string, std::string, uint32_t, bool)> image_available_callback_{};
  // (error_type): "not_linked" (expected for a new/unapproved device, not
  // really a failure), "network" (request/transport failure), "server_error"
  // (the response's own status field indicates an error), or "parse_error"
  // (malformed/unexpected response body).
  CallbackManager<void(std::string)> error_callback_{};
};

/// Backs the `trmnl.update` action.
template<typename... Ts> class TrmnlUpdateAction final : public Action<Ts...> {
 public:
  explicit TrmnlUpdateAction(TrmnlComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->check_for_update(); }

 protected:
  TrmnlComponent *parent_;
};

/// Backs the `trmnl.trigger_special_function` action.
template<typename... Ts> class TrmnlTriggerSpecialFunctionAction final : public Action<Ts...> {
 public:
  explicit TrmnlTriggerSpecialFunctionAction(TrmnlComponent *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->trigger_special_function(); }

 protected:
  TrmnlComponent *parent_;
};

}  // namespace esphome::trmnl
