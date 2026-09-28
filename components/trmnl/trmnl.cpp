#include "trmnl.h"

#include <cstdlib>
#include <cstring>

#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome::trmnl {

static const char *const TAG = "trmnl";
// The BYOD /api/display and /api/setup responses are small JSON objects
// (image_url, filename, refresh_rate, api_key, ...); 4kB is generous headroom
// without risking a large heap allocation on constrained boards.
static constexpr size_t MAX_RESPONSE_SIZE = 4096;
// Identifies this component's storage slot; XORed with a hash of base_url so
// multiple `trmnl` instances (MULTI_CONF) don't collide.
static constexpr uint32_t PREF_TYPE = 0x7924D1C7;

void TrmnlComponent::setup() { this->load_state_(); }

void TrmnlComponent::load_state_() {
  this->pref_ = global_preferences->make_preference<TrmnlStoredState>(PREF_TYPE ^ fnv1_hash(this->base_url_), true);
  TrmnlStoredState stored{};
  if (this->pref_.load(&stored)) {
    if (this->api_key_.empty() && stored.api_key[0] != '\0') {
      // Defensive: guarantee null-termination regardless of what was stored.
      stored.api_key[sizeof(stored.api_key) - 1] = '\0';
      this->api_key_ = stored.api_key;
      ESP_LOGD(TAG, "Restored API key from a previous /api/setup registration");
    }
    if (stored.friendly_id[0] != '\0') {
      stored.friendly_id[sizeof(stored.friendly_id) - 1] = '\0';
      this->friendly_id_ = stored.friendly_id;
    }
    this->last_refresh_rate_ = stored.refresh_rate;
    this->last_image_url_hash_ = stored.image_url_hash;
  }
}

void TrmnlComponent::save_state_() {
  TrmnlStoredState stored{};
  strncpy(stored.api_key, this->api_key_.c_str(), sizeof(stored.api_key) - 1);
  strncpy(stored.friendly_id, this->friendly_id_.c_str(), sizeof(stored.friendly_id) - 1);
  stored.refresh_rate = this->last_refresh_rate_;
  stored.image_url_hash = this->last_image_url_hash_;
  if (!this->pref_.save(&stored)) {
    ESP_LOGW(TAG, "Failed to persist state to flash");
  }
  global_preferences->sync();
}

void TrmnlComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "TRMNL:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Model: %s", this->model_.c_str());
  ESP_LOGCONFIG(TAG, "  API key configured: %s", YESNO(!this->api_key_.empty()));
  if (!this->friendly_id_.empty()) {
    // Only ever surfaced here, not via a trigger -- this is the code you
    // claim the device with on the TRMNL dashboard, and a deep-sleeping
    // device may only register once, so it needs to be recoverable after
    // the fact rather than only visible for one boot.
    ESP_LOGCONFIG(TAG, "  Registered as: %s", this->friendly_id_.c_str());
  }
  if (this->width_ > 0 && this->height_ > 0) {
    ESP_LOGCONFIG(TAG, "  Reported size: %dx%d", this->width_, this->height_);
  }
#ifdef USE_SENSOR
  ESP_LOGCONFIG(TAG, "  Battery voltage sensor: %s", YESNO(this->voltage_sensor_ != nullptr));
  ESP_LOGCONFIG(TAG, "  Battery level sensor: %s", YESNO(this->battery_level_sensor_ != nullptr));
#endif
#ifdef USE_BINARY_SENSOR
  ESP_LOGCONFIG(TAG, "  Charging binary sensor: %s", YESNO(this->charging_binary_sensor_ != nullptr));
#endif
}

std::vector<http_request::Header> TrmnlComponent::build_headers_(bool for_setup) {
  std::vector<http_request::Header> headers;
  // The BYOD contract identifies the device by its MAC address in the `ID`
  // header (see https://docs.trmnl.com/go/diy/introduction).
  headers.push_back({"ID", get_mac_address_pretty()});
  headers.push_back({"FW-Version", ESPHOME_VERSION});
  headers.push_back({"Model", this->model_});

  // /api/setup is how a device without a key registers itself; it must not
  // send an (empty) Access-Token, and none of the runtime state below is
  // relevant yet.
  if (for_setup) {
    return headers;
  }

  if (!this->api_key_.empty()) {
    headers.push_back({"Access-Token", this->api_key_});
  }
  if (this->width_ > 0) {
    headers.push_back({"Width", std::to_string(this->width_)});
  }
  if (this->height_ > 0) {
    headers.push_back({"Height", std::to_string(this->height_)});
  }
  // Reported back to the server so it knows the cadence the device is
  // currently operating at (0 if this device has never completed an update).
  headers.push_back({"Refresh-Rate", std::to_string(this->last_refresh_rate_)});
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    headers.push_back({"RSSI", std::to_string(wifi::global_wifi_component->wifi_rssi())});
  }
#endif
#ifdef USE_SENSOR
  if (this->voltage_sensor_ != nullptr && this->voltage_sensor_->has_state()) {
    headers.push_back({"Battery-Voltage", str_sprintf("%.2f", this->voltage_sensor_->get_state())});
  }
  if (this->battery_level_sensor_ != nullptr && this->battery_level_sensor_->has_state()) {
    headers.push_back({"Percent-Charged", str_sprintf("%.0f", this->battery_level_sensor_->get_state())});
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (this->charging_binary_sensor_ != nullptr && this->charging_binary_sensor_->has_state()) {
    headers.push_back({"Battery-Charging", this->charging_binary_sensor_->state ? "1" : "0"});
  }
#endif
  // Requests a TRMNL "special function" for this cycle -- see
  // trigger_special_function() and docs/trmnl.mdx. The official firmware
  // only ever sends this as a boolean flag, never a specific function name;
  // which one runs is decided server-side.
  if (this->special_function_requested_) {
    headers.push_back({"special_function", "true"});
  }
  return headers;
}

bool TrmnlComponent::update_image_hash_(const std::string &image_url, bool &needs_save) {
  if (image_url.empty()) {
    return false;
  }
  uint32_t url_hash = fnv1_hash(image_url);
  bool changed = url_hash != this->last_image_url_hash_;
  if (changed) {
    this->last_image_url_hash_ = url_hash;
    needs_save = true;
  }
  return changed;
}

RegisterOutcome TrmnlComponent::register_device_() {
  auto headers = this->build_headers_(true);
  std::string url = this->base_url_ + "/api/setup";
  auto container = this->parent_->get(url, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "Registration request to %s failed to start", url.c_str());
    return RegisterOutcome::NETWORK_ERROR;
  }
  if (!http_request::is_success(container->status_code)) {
    ESP_LOGW(TAG, "Registration failed: server returned HTTP %d", container->status_code);
    container->end();
    return RegisterOutcome::NETWORK_ERROR;
  }

  size_t len = container->content_length;
  if (len == 0 || len > MAX_RESPONSE_SIZE) {
    len = MAX_RESPONSE_SIZE;
  }
  std::vector<uint8_t> buffer(len);
  auto result = http_request::http_read_fully(container.get(), buffer.data(), len, 512, this->parent_->get_timeout());
  size_t bytes_read = container->get_bytes_read();
  container->end();
  if (result.status != http_request::HttpReadStatus::OK) {
    ESP_LOGW(TAG, "Failed reading registration response body (status=%d)", static_cast<int>(result.status));
    return RegisterOutcome::NETWORK_ERROR;
  }

  RegisterOutcome outcome = RegisterOutcome::PARSE_ERROR;
  bool parsed = json::parse_json(buffer.data(), bytes_read, [this, &outcome](JsonObject root) -> bool {
    // Mirrors the official firmware's ApiSetupResponse parsing: status 200
    // means the server has assigned this device an API key; anything else
    // means it's waiting to be linked, which is an expected state, not an
    // error.
    int status = root["status"] | 0;
    if (status != 200) {
      ESP_LOGI(TAG, "Not registered yet (status %d)", status);
      outcome = RegisterOutcome::NOT_LINKED;
      return true;
    }
    if (!root["api_key"].is<const char *>() || root["api_key"].as<const char *>()[0] == '\0') {
      // A 200 with no usable api_key is a genuinely unexpected/malformed
      // response, not "waiting to be linked" -- surface it as an error
      // rather than silently treating it the same as NOT_LINKED.
      ESP_LOGW(TAG, "Registration response had status 200 but no usable api_key");
      outcome = RegisterOutcome::PARSE_ERROR;
      return true;
    }
    this->api_key_ = root["api_key"].as<const char *>();

    if (root["friendly_id"].is<const char *>()) {
      this->friendly_id_ = root["friendly_id"].as<const char *>();
    }
    if (this->friendly_id_.empty()) {
      ESP_LOGI(TAG, "Registered with server");
    } else {
      ESP_LOGI(TAG, "Registered with server -- claim it on the TRMNL dashboard with ID: %s",
               this->friendly_id_.c_str());
    }

    // The setup response may also include an initial "please link this
    // device" screen -- route it through the same path as a normal image so
    // whatever the user already wired to on_image_available draws it, rather
    // than needing a second, setup-specific automation for the same thing.
    bool needs_save = true;  // always persist at least the new api_key/friendly_id
    std::string image_url;
    if (root["image_url"].is<const char *>()) {
      image_url = root["image_url"].as<const char *>();
    }
    bool image_changed = this->update_image_hash_(image_url, needs_save);
    if (needs_save) {
      this->save_state_();
    }
    std::string filename;
    if (root["filename"].is<const char *>()) {
      filename = root["filename"].as<const char *>();
    }
    this->image_available_callback_.call(image_url, filename, 0, image_changed);

    outcome = RegisterOutcome::REGISTERED;
    return true;
  });

  if (!parsed) {
    ESP_LOGW(TAG, "Failed to parse registration response");
    return RegisterOutcome::PARSE_ERROR;
  }
  return outcome;
}

void TrmnlComponent::check_for_update() {
  if (this->base_url_.empty()) {
    ESP_LOGE(TAG, "base_url is not set");
    this->error_callback_.call("network");
    return;
  }

  if (this->api_key_.empty()) {
    switch (this->register_device_()) {
      case RegisterOutcome::REGISTERED:
        // Registration can return a setup screen. Let the caller finish
        // handling it before requesting the first playlist image.
        return;
      case RegisterOutcome::NOT_LINKED:
        this->error_callback_.call("not_linked");
        return;
      case RegisterOutcome::NETWORK_ERROR:
        this->error_callback_.call("network");
        return;
      case RegisterOutcome::PARSE_ERROR:
        this->error_callback_.call("parse_error");
        return;
    }
  }

  auto headers = this->build_headers_(false);
  // Consumed whether or not the request below actually succeeds -- a request
  // that never reaches the server shouldn't leave the flag armed to silently
  // retry on some later, unrelated update.
  this->special_function_requested_ = false;
  std::string url = this->base_url_ + "/api/display";
  auto container = this->parent_->get(url, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "Request to %s failed to start", url.c_str());
    this->error_callback_.call("network");
    return;
  }

  if (!http_request::is_success(container->status_code)) {
    ESP_LOGW(TAG, "Server returned HTTP %d", container->status_code);
    container->end();
    this->error_callback_.call("network");
    return;
  }

  size_t len = container->content_length;
  if (len == 0 || len > MAX_RESPONSE_SIZE) {
    len = MAX_RESPONSE_SIZE;
  }
  std::vector<uint8_t> buffer(len);
  auto result = http_request::http_read_fully(container.get(), buffer.data(), len, 512, this->parent_->get_timeout());
  size_t bytes_read = container->get_bytes_read();
  container->end();

  if (result.status != http_request::HttpReadStatus::OK) {
    ESP_LOGW(TAG, "Failed reading response body (status=%d)", static_cast<int>(result.status));
    this->error_callback_.call("network");
    return;
  }

  this->handle_response_(buffer.data(), bytes_read);
}

void TrmnlComponent::handle_response_(const uint8_t *data, size_t len) {
  bool parsed = json::parse_json(data, len, [this](JsonObject root) -> bool {
    // Per https://docs.trmnl.com/go/private-api/screens: status 0 (or absent,
    // for BYOS servers that omit it) means success, 202 means the device
    // isn't linked to a playlist yet.
    int status = root["status"] | 0;

    uint32_t refresh_rate = 0;
    JsonVariantConst rr = root["refresh_rate"];
    if (!rr.isNull()) {
      // The official API has been observed returning refresh_rate as both a
      // JSON number and a numeric string, so handle both.
      if (rr.is<const char *>()) {
        refresh_rate = static_cast<uint32_t>(strtoul(rr.as<const char *>(), nullptr, 10));
      } else {
        refresh_rate = rr.as<uint32_t>();
      }
    }

    bool needs_save = false;

    std::string action;
    if (root["action"].is<const char *>()) {
      action = root["action"].as<const char *>();
    }

    if (!action.empty()) {
      // A special function ran server-side (triggered by a previous
      // trigger_special_function() call) -- see
      // https://help.trmnl.com/en/articles/9672080-special-functions and
      // docs/trmnl.mdx for what this component does and doesn't cover. The
      // device doesn't need to know *which* function ran, just whatever
      // image/refresh_rate came back with it, so this reports through the
      // exact same event as a normal update rather than a separate one.
      std::string image_url;
      if (root["image_url"].is<const char *>()) {
        image_url = root["image_url"].as<const char *>();
      }
      bool image_changed = this->update_image_hash_(image_url, needs_save);
      if (refresh_rate > 0 && refresh_rate != this->last_refresh_rate_) {
        this->last_refresh_rate_ = refresh_rate;
        needs_save = true;
      }
      ESP_LOGI(TAG, "Special function result: %s (refresh_rate=%u)", action.c_str(), refresh_rate);
      if (needs_save) {
        this->save_state_();
      }
      this->image_available_callback_.call(image_url, "", refresh_rate, image_changed);
      return true;
    }

    if (status == 202) {
      ESP_LOGI(TAG, "Device is not linked to a playlist yet (status 202)");
      this->error_callback_.call("not_linked");
      return true;
    }
    if (status != 0 && status != 200) {
      ESP_LOGW(TAG, "Server reported status %d", status);
      this->error_callback_.call("server_error");
      return true;
    }
    if (!root["image_url"].is<const char *>()) {
      ESP_LOGW(TAG, "Response did not contain an image_url");
      this->error_callback_.call("parse_error");
      return true;
    }

    std::string image_url = root["image_url"].as<const char *>();
    // Different BYOD/BYOS implementations have been observed using either
    // `filename` or `image_name` for the same field; accept both.
    std::string filename;
    if (root["filename"].is<const char *>()) {
      filename = root["filename"].as<const char *>();
    } else if (root["image_name"].is<const char *>()) {
      filename = root["image_name"].as<const char *>();
    }
    // image_changed is a hint, not a guarantee: it's a straight comparison
    // against the URL we last saw, computed here because it costs nothing
    // and every consumer would otherwise need its own persisted state to
    // answer the same question. A server that reuses one fixed URL for
    // genuinely different content would make this hint wrong -- it's
    // still safe to ignore it and always re-download.
    bool image_changed = this->update_image_hash_(image_url, needs_save);
    if (refresh_rate > 0 && refresh_rate != this->last_refresh_rate_) {
      this->last_refresh_rate_ = refresh_rate;
      needs_save = true;
    }
    ESP_LOGD(TAG, "Image available: %s (refresh_rate=%u, changed=%s)", image_url.c_str(), refresh_rate,
             YESNO(image_changed));
    if (needs_save) {
      this->save_state_();
    }
    this->image_available_callback_.call(image_url, filename, refresh_rate, image_changed);
    return true;
  });

  if (!parsed) {
    ESP_LOGW(TAG, "Failed to parse JSON response");
    this->error_callback_.call("parse_error");
  }
}

}  // namespace esphome::trmnl
