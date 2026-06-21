/**
 * @file autoterm_uart.cpp
 * @brief Implementation of the Autoterm UART bridge component.
 */

#include "autoterm_uart.h"
#include <ctime>

namespace esphome {
namespace autoterm_uart {

using namespace esphome::uart;
using namespace esphome::sensor;

// ===================
// Forward and sniff implementation
// ===================
void AutotermUART::forward_and_sniff(uart::UARTComponent *src, uart::UARTComponent *dst, const char *tag,
                       bool from_display) {
  if (!src || !dst) return;

  auto &buffer = from_display ? display_to_heater_buffer_ : heater_to_display_buffer_;
  auto &frame_start = from_display ? display_frame_start_millis_ : heater_frame_start_millis_;

  while (src->available()) {
    uint8_t b;
    if (!src->read_byte(&b)) break;

    buffer.push_back(b);

    if (from_display) {
      last_display_activity_ = millis();
    } else {
      last_heater_activity_ = millis();
    }

    // Track frame start time for timeout detection
    if (buffer.size() == 1 && b == FRAME_HEADER) {
      frame_start = millis();
    }

    // Forward bytes before header directly
    while (!buffer.empty() && buffer[0] != FRAME_HEADER) {
      dst->write_byte(buffer[0]);
      buffer.erase(buffer.begin());
    }

    // Frame timeout: if we started a frame but haven't completed it, resync
    if (!buffer.empty() && buffer[0] == FRAME_HEADER && frame_start > 0) {
      uint32_t now = millis();
      if (check_frame_timeout_(frame_start, now)) {
        ESP_LOGW("autoterm_uart", "[%s] Frame timeout after %u ms, resyncing",
                 tag, static_cast<unsigned>(now - frame_start));
        resync_uart_buffer_(buffer);
        frame_start = 0;
        continue;
      }
    }

    while (true) {
      if (buffer.empty())
        break;
      if (buffer[0] != FRAME_HEADER)
        break;
      if (buffer.size() < 3)
        break;

      uint8_t len = buffer[2];
      size_t total = 5 + static_cast<size_t>(len) + 2;
      if (buffer.size() < total)
        break;

      frame_start = 0;  // Frame complete, reset timeout
      std::vector<uint8_t> frame(buffer.begin(), buffer.begin() + total);
      process_frame_(std::move(frame), dst, tag, from_display);
      buffer.erase(buffer.begin(), buffer.begin() + total);
    }

    // Buffer overflow protection: if buffer grows too large without a valid frame
    if (buffer.size() > 64) {
      ESP_LOGW("autoterm_uart", "[%s] Buffer overflow (%u bytes), flushing", tag, buffer.size());
      for (uint8_t byte : buffer)
        dst->write_byte(byte);
      buffer.clear();
      frame_start = 0;
    }
  }
}

// CRC16 (Modbus)
bool AutotermUART::validate_crc(const std::vector<uint8_t> &data) {
  if (data.size() < 3) return false;
  uint16_t expected = crc16_modbus_(data.data(), data.size() - 2);
  uint16_t recv_crc = (data[data.size() - 2] << 8) | data[data.size() - 1];
  return expected == recv_crc;
}

// Validate frame structure
bool AutotermUART::validate_frame_structure_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 5) return false;
  if (frame[0] != FRAME_HEADER) return false;
  // Accept all known device IDs: display(0x03), heater(0x04), diagnostic(0x02), boot(0x00)
  if (frame[1] != DEVICE_DISPLAY && frame[1] != DEVICE_HEATER &&
      frame[1] != DEVICE_DIAG && frame[1] != DEVICE_BOOT) return false;
  uint8_t len = frame[2];
  size_t expected_total = 5 + static_cast<size_t>(len) + 2;
  if (frame.size() != expected_total) return false;
  return true;
}

void AutotermUART::log_frame(const char *tag, const std::vector<uint8_t> &data) {
  std::string hex;
  char temp[6];
  for (auto v : data) {
    sprintf(temp, "%02X ", v);
    hex += temp;
  }
  ESP_LOGD("autoterm_uart", "[%s] Frame (%u bytes): %s", tag, (unsigned)data.size(), hex.c_str());
}

// ===================
// AutotermFanLevelNumber implementation
// ===================

// Number changed -> Send level
void AutotermFanLevelNumber::control(float value) {
  publish_state(value);
  if (parent_) parent_->send_fan_mode(true, (int)value);
}

// ===================
// AutotermTempSourceSelect implementations
// ===================

void AutotermTempSourceSelect::set_parent(AutotermUART *parent) {
  parent_ = parent;
  this->traits.set_options({"Intern", "Panel", "Extern", "Home Assistant"});
}

const char *AutotermTempSourceSelect::option_from_source_(uint8_t source) const {
  switch (source) {
    case 1:
      return "Intern";
    case 2:
      return "Panel";
    case 3:
      return "Extern";
    case 4:
      return "Home Assistant";
    default:
      return "Intern";
  }
}

uint8_t AutotermTempSourceSelect::source_from_option_(const std::string &option) const {
  if (option == "Intern" || option == "1")
    return 1;
  if (option == "Panel" || option == "2")
    return 2;
  if (option == "Extern" || option == "3")
    return 3;
  if (option == "Home Assistant" || option == "4")
    return 4;
  return 0;
}

void AutotermTempSourceSelect::publish_for_source(uint8_t source) {
  this->publish_state(option_from_source_(source));
}

void AutotermTempSourceSelect::control(const std::string &value) {
  if (parent_ == nullptr) {
    this->publish_state(value);
    return;
  }
  uint8_t src = source_from_option_(value);
  if (src == 0) {
    ESP_LOGW("autoterm_uart", "Temperature source select received unknown option '%s'", value.c_str());
    parent_->publish_temp_source_select_(parent_->get_manual_temp_source());
    return;
  }
  parent_->set_temp_source_from_select(src);
}

// ===================
// Button implementations
// ===================

void AutotermUnlockButton::press_action() {
  if (parent_) {
    ESP_LOGW("autoterm_uart", "Button: Clear Lockout (unlock) + clear emergency state");
    parent_->send_unlock_();
    // Also clear emergency shutdown state to allow restart
    if (parent_->emergency_shutdown_active_) {
      parent_->emergency_shutdown_active_ = false;
      ESP_LOGI("autoterm_uart", "Emergency state cleared by user");
      if (parent_->error_text_sensor_) parent_->error_text_sensor_->publish_state("cleared by user");
    }
  }
}

void AutotermPrimePumpButton::press_action() {
  if (parent_) {
    ESP_LOGI("autoterm_uart", "Button: Prime pump at %u Hz", static_cast<unsigned>(prime_frequency_));
    parent_->send_prime_pump_(prime_frequency_);
  }
}

void AutotermStatusReportButton::press_action() {
  if (!parent_) return;
  ESP_LOGI("autoterm_uart", "Button: Status Report requested");
  // Request all diagnostic data
  parent_->send_version_request_();
  parent_->send_report_request_();
  parent_->send_status_request_();
  parent_->send_diagnostic_mode_(true);
  // Build status report text
  static char report_buf[256];
  snprintf(report_buf, sizeof(report_buf),
           "Firmware: %s | Runtime: %.1fh | Fuel: %.2fL | Starts: %u | Error: %u (%s) | Uptime: %lus",
           parent_->firmware_version_sensor_ ? parent_->firmware_version_sensor_->state.c_str() : "?",
           parent_->runtime_hours_,
           parent_->total_fuel_liters_,
           parent_->history_total_starts_,
           parent_->last_error_code_,
           parent_->error_code_to_text_(parent_->last_error_code_),
           millis() / 1000);
  if (parent_->error_log_sensor_) parent_->error_log_sensor_->publish_state(report_buf);
  ESP_LOGI("autoterm_uart", "Status Report: %s", report_buf);
}

// ===================
// AutotermUART setter implementations (non-inline)
// ===================

void AutotermUART::set_runtime_hours_sensor(sensor::Sensor *s) {
  runtime_hours_sensor_ = s;
  if (runtime_hours_sensor_ != nullptr && runtime_loaded_)
    publish_runtime_hours_(true);
}

void AutotermUART::set_session_runtime_sensor(sensor::Sensor *s) {
  session_runtime_sensor_ = s;
  if (session_runtime_sensor_ != nullptr && runtime_tracking_initialized_)
    publish_session_runtime_(true);
}

void AutotermUART::set_panel_temp_override_sensor(sensor::Sensor *s) {
  panel_temp_override_sensor_ = s;
  if (panel_temp_override_sensor_ != nullptr) {
    panel_temp_override_sensor_->add_on_state_callback([this](float value) {
      this->panel_temp_override_value_c_ = value;
    });
    if (panel_temp_override_sensor_->has_state())
      panel_temp_override_value_c_ = panel_temp_override_sensor_->state;
  }
}

void AutotermUART::set_temp_source_select(AutotermTempSourceSelect *select) {
  temp_source_select_ = select;
  if (temp_source_select_ != nullptr) {
    temp_source_select_->set_parent(this);
    uint8_t initial = manual_temp_source_active_ ? manual_temp_source_value_
                                                : (settings_valid_ ? clamp_temp_source_(settings_.temperature_source)
                                                                   : static_cast<uint8_t>(1));
    publish_temp_source_select_(initial);
  }
}

void AutotermUART::set_climate(AutotermClimate *climate) {
  climate_ = climate;
  if (climate_ != nullptr) {
    climate_->set_parent(this);
    if (settings_valid_)
      climate_->handle_settings_update(settings_, false);
  }
}

// ===================
// Temperature source management
// ===================

void AutotermUART::set_temp_source_from_select(uint8_t source) {
  uint8_t clamped = clamp_temp_source_(source);
  bool changed = !manual_temp_source_active_ || manual_temp_source_value_ != clamped;
  manual_temp_source_active_ = true;
  manual_temp_source_value_ = clamped;
  publish_temp_source_select_(clamped);
  if (changed) {
    ESP_LOGI("autoterm_uart", "Temperature source set via select to %u", static_cast<unsigned>(clamped));
    if (climate_ != nullptr)
      climate_->publish_state();
  }
}

void AutotermUART::apply_temp_source_from_settings(uint8_t source) {
  uint8_t clamped = clamp_temp_source_(source);
  settings_.temperature_source = clamped;
  if (!manual_temp_source_active_)
    publish_temp_source_select_(clamped);
}

uint8_t AutotermUART::get_effective_temp_source() const {
  if (manual_temp_source_active_ && manual_temp_source_value_ >= 1 && manual_temp_source_value_ <= 4)
    return manual_temp_source_value_;
  if (settings_valid_)
    return clamp_temp_source_(settings_.temperature_source);
  return 1;
}

float AutotermUART::get_temperature_for_source(uint8_t source) const {
  uint8_t clamped = clamp_temp_source_(source);
  float value = NAN;
  switch (clamped) {
    case 1:
      value = last_internal_temp_c_;
      break;
    case 2:
      value = panel_temp_last_value_c_;
      break;
    case 3:
      value = last_external_temp_c_;
      break;
    case 4:
      value = panel_temp_override_value_c_;
      break;
    default:
      value = last_internal_temp_c_;
      break;
  }
  if (std::isfinite(value))
    return value;
  if (std::isfinite(last_internal_temp_c_))
    return last_internal_temp_c_;
  if (std::isfinite(panel_temp_last_value_c_))
    return panel_temp_last_value_c_;
  if (std::isfinite(last_external_temp_c_))
    return last_external_temp_c_;
  return NAN;
}

// ===================
// Runtime tracking
// ===================

void AutotermUART::advance_runtime_time_(uint32_t now) {
  if (!runtime_tracking_initialized_) {
    last_runtime_millis_ = now;
    runtime_tracking_initialized_ = true;
    return;
  }

  uint32_t delta = now - last_runtime_millis_;
  last_runtime_millis_ = now;

  if (!heater_running_ || delta == 0)
    return;

  runtime_hours_ += static_cast<float>(delta) / 3600000.0f;
  session_runtime_hours_ += static_cast<float>(delta) / 3600000.0f;
  runtime_dirty_ = true;
  publish_runtime_hours_();
  publish_session_runtime_();
}

void AutotermUART::set_heater_running_state_(bool running) {
  if (heater_running_ == running)
    return;

  uint32_t now = millis();
  advance_runtime_time_(now);
  heater_running_ = running;
  last_runtime_millis_ = now;

  if (heater_running_) {
    session_runtime_hours_ = 0.0f;
    session_runtime_last_published_ = NAN;
    publish_session_runtime_(true);
    heater_stopped_ms_ = 0;  // Reset stop timer
  } else {
    heater_stopped_ms_ = now;  // Record when heater stopped
    publish_runtime_hours_(true);
    publish_session_runtime_(true);
    maybe_save_runtime_hours_(now, true);
  }
}

void AutotermUART::publish_runtime_hours_(bool force) {
  if (!runtime_loaded_ || runtime_hours_sensor_ == nullptr)
    return;

  bool should_publish = force;
  if (!should_publish) {
    if (std::isnan(runtime_hours_last_published_) ||
        std::fabs(runtime_hours_ - runtime_hours_last_published_) >= 0.001f) {
      should_publish = true;
    }
  }

  if (!should_publish)
    return;

  runtime_hours_sensor_->publish_state(runtime_hours_);
  runtime_hours_last_published_ = runtime_hours_;
}

void AutotermUART::publish_session_runtime_(bool force) {
  if (session_runtime_sensor_ == nullptr)
    return;

  bool should_publish = force;
  if (!should_publish) {
    if (std::isnan(session_runtime_last_published_) ||
        std::fabs(session_runtime_hours_ - session_runtime_last_published_) >= 0.001f) {
      should_publish = true;
    }
  }

  if (!should_publish)
    return;

  session_runtime_sensor_->publish_state(session_runtime_hours_);
  session_runtime_last_published_ = session_runtime_hours_;
}

void AutotermUART::maybe_save_runtime_hours_(uint32_t now, bool force) {
  if (!runtime_dirty_ || !runtime_storage_initialized_)
    return;

  if (!force && (now - last_runtime_save_millis_) < 60000)
    return;

  if (runtime_hours_pref_.save(&runtime_hours_)) {
    runtime_dirty_ = false;
    last_runtime_save_millis_ = now;
  }
}

void AutotermUART::save_burn_cycle_hours_() {
  if (!burn_cycle_hours_dirty_ || !runtime_storage_initialized_)
    return;

  if (burn_cycle_hours_pref_.save(&last_burn_cycle_hours_)) {
    burn_cycle_hours_dirty_ = false;
  }
}

// ===================
// Maintenance counter tracking
// ===================

void AutotermUART::reset_oil_maintenance_() {
  oil_reset_hours_ = runtime_hours_;
  if (runtime_storage_initialized_)
    oil_reset_pref_.save(&oil_reset_hours_);
  maintenance_alert_oil_ = false;
  publish_maintenance_counters_();
  publish_maintenance_(true);
  ESP_LOGI("autoterm_uart", "Maintenance: oil (combustion chamber) counter reset at %.1fh", runtime_hours_);
}

void AutotermUART::reset_filter_maintenance_() {
  filter_reset_hours_ = runtime_hours_;
  if (runtime_storage_initialized_)
    filter_reset_pref_.save(&filter_reset_hours_);
  maintenance_alert_filter_ = false;
  publish_maintenance_counters_();
  publish_maintenance_(true);
  ESP_LOGI("autoterm_uart", "Maintenance: fuel filter counter reset at %.1fh", runtime_hours_);
}

void AutotermUART::reset_glow_maintenance_() {
  glow_reset_hours_ = runtime_hours_;
  if (runtime_storage_initialized_)
    glow_reset_pref_.save(&glow_reset_hours_);
  maintenance_alert_glow_ = false;
  publish_maintenance_counters_();
  publish_maintenance_(true);
  ESP_LOGI("autoterm_uart", "Maintenance: glow plug counter reset at %.1fh", runtime_hours_);
}

void AutotermUART::publish_maintenance_counters_() {
  // Oil (combustion chamber cleaning)
  float oil_since = runtime_hours_ - oil_reset_hours_;
  float oil_remaining = maintenance_oil_hrs_ - oil_since;
  if (maintenance_oil_since_sensor_) maintenance_oil_since_sensor_->publish_state(oil_since);
  if (maintenance_oil_remaining_sensor_) maintenance_oil_remaining_sensor_->publish_state(oil_remaining);

  // Fuel filter
  float filter_since = runtime_hours_ - filter_reset_hours_;
  float filter_remaining = maintenance_filter_hrs_ - filter_since;
  if (maintenance_filter_since_sensor_) maintenance_filter_since_sensor_->publish_state(filter_since);
  if (maintenance_filter_remaining_sensor_) maintenance_filter_remaining_sensor_->publish_state(filter_remaining);

  // Glow plug
  float glow_since = runtime_hours_ - glow_reset_hours_;
  float glow_remaining = maintenance_glow_hrs_ - glow_since;
  if (maintenance_glow_since_sensor_) maintenance_glow_since_sensor_->publish_state(glow_since);
  if (maintenance_glow_remaining_sensor_) maintenance_glow_remaining_sensor_->publish_state(glow_remaining);
}

void AutotermUART::save_maintenance_counters_() {
  if (!runtime_storage_initialized_)
    return;
  oil_reset_pref_.save(&oil_reset_hours_);
  filter_reset_pref_.save(&filter_reset_hours_);
  glow_reset_pref_.save(&glow_reset_hours_);
}

// Button implementations
void AutotermResetOilButton::press_action() {
  if (parent_) parent_->reset_oil_maintenance_();
}

void AutotermResetFilterButton::press_action() {
  if (parent_) parent_->reset_filter_maintenance_();
}

void AutotermResetGlowButton::press_action() {
  if (parent_) parent_->reset_glow_maintenance_();
}

bool AutotermUART::is_heater_active_status_(uint16_t status_code) const {
  // Return false for non-active states: sleep, standby, cooling, idle vent, shutdown
  // This ensures runtime hours are only counted during actual heating
  if (status_code == STATUS_SLEEP || status_code == STATUS_STANDBY ||
      status_code == STATUS_COOLING || status_code == STATUS_IDLE_VENT ||
      status_code == STATUS_SHUTDOWN)
    return false;
  return true;
}

// ===================
// Reliability: CRC resync
// ===================
void AutotermUART::resync_uart_buffer_(std::vector<uint8_t> &buffer) {
  // Find next valid header byte and discard everything before it
  for (size_t i = 0; i < buffer.size(); i++) {
    if (buffer[i] == FRAME_HEADER) {
      if (i > 0) {
        buffer.erase(buffer.begin(), buffer.begin() + i);
        crc_resync_count_++;
        ESP_LOGD("autoterm_uart", "UART buffer resync: skipped %u bytes (resync #%u)",
                 static_cast<unsigned>(i), crc_resync_count_);
      }
      return;
    }
  }
  // No header found, clear entire buffer
  buffer.clear();
}

// ===================
// Reliability: Frame timeout
// ===================
bool AutotermUART::check_frame_timeout_(uint32_t frame_start, uint32_t now) const {
  if (frame_start == 0) return false;
  return (now - frame_start) > FRAME_TIMEOUT_MS;
}

// ===================
// Reliability: UART loss detection
// ===================
void AutotermUART::check_uart_loss_(uint32_t now) {
  if (heater_running_ && last_heater_activity_ > 0 &&
      (now - last_heater_activity_) > UART_LOST_TIMEOUT_MS) {
    if (heater_connected_) {
      heater_connected_ = false;
      ESP_LOGE("autoterm_uart", "HEATER UART LOST! No data received for %u ms",
               static_cast<unsigned>(UART_LOST_TIMEOUT_MS));
    }
  } else if (!heater_connected_ && last_heater_activity_ > 0) {
    heater_connected_ = true;
    // After reconnection, wait 2 seconds before sending commands
    // to let the UART bus stabilize and avoid corrupt frames
    last_command_millis_ = now;
    ESP_LOGI("autoterm_uart", "Heater UART connection restored (commands paused 2s)");
  }
}

// ===================
// Rate limiting
// ===================
bool AutotermUART::check_command_rate_limit_(uint32_t now) {
  if (last_command_millis_ > 0 && (now - last_command_millis_) < COMMAND_RATE_LIMIT_MS) {
    ESP_LOGW("autoterm_uart", "Command rate limit: %.0fms since last command (min %u ms)",
             (now - last_command_millis_), static_cast<unsigned>(COMMAND_RATE_LIMIT_MS));
    return false;
  }
  return true;
}

// ===================
// Fuel consumption estimation
// ===================
void AutotermUART::load_fuel_data_() {
  if (global_preferences != nullptr) {
    fuel_pref_ = global_preferences->make_preference<float>(fnv1_hash("autoterm_fuel_liters"));
    fuel_pref_.load(&total_fuel_liters_);
  }
}

void AutotermUART::update_fuel_consumption_(float pump_freq) {
  // Cap pump frequency to reduce mechanical stress on fuel pump
  float capped_freq = std::min(pump_freq, max_pump_freq_hz_);

  // Smooth pump frequency over 30-second window to reduce mechanical stress
  // Rapid frequency changes cause diaphragm fatigue
  uint32_t now = millis();
  if (last_pump_smooth_ms_ > 0) {
    float dt = static_cast<float>(now - last_pump_smooth_ms_) / 1000.0f;
    if (dt > 0.0f && dt < 60.0f) {
      // Exponential moving average with 30-second time constant
      float alpha = dt / (30.0f + dt);
      smoothed_pump_freq_ = smoothed_pump_freq_ * (1.0f - alpha) + capped_freq * alpha;
    } else {
      smoothed_pump_freq_ = capped_freq;
    }
  } else {
    smoothed_pump_freq_ = capped_freq;
  }
  last_pump_smooth_ms_ = now;

  // Estimate L/h from smoothed pump frequency
  fuel_consumption_lph_ = smoothed_pump_freq_ * FUEL_LITERS_PER_HZ;

  // Accumulate fuel consumed using ACTUAL elapsed time (not hardcoded interval)
  uint32_t now = millis();
  if (heater_running_ && pump_freq > 0 && last_fuel_update_ms_ > 0) {
    float elapsed_s = static_cast<float>(now - last_fuel_update_ms_) / 1000.0f;
    if (elapsed_s > 0.0f && elapsed_s < 30.0f) {  // Sanity check: max 30s between updates
      float consumed = fuel_consumption_lph_ * (elapsed_s / 3600.0f);
      total_fuel_liters_ += consumed;
      daily_fuel_liters_ += consumed;
      fuel_dirty_ = true;
    }
  }
  last_fuel_update_ms_ = now;

  // Daily reset at midnight
  struct tm timeinfo;
  if (now_local(&timeinfo)) {
    int today = timeinfo.tm_mday;
    if (last_fuel_reset_day_ >= 0 && today != last_fuel_reset_day_) {
      ESP_LOGI("autoterm_uart", "Daily fuel reset: yesterday=%.2fL", daily_fuel_liters_);
      daily_fuel_liters_ = 0.0f;
    }
    last_fuel_reset_day_ = today;
  }
}

void AutotermUART::publish_fuel_consumption_(bool force) {
  if (!fuel_consumption_sensor_) return;

  bool should_publish = force;
  if (!should_publish) {
    if (std::isnan(fuel_consumption_last_published_) ||
        std::fabs(fuel_consumption_lph_ - fuel_consumption_last_published_) >= 0.01f) {
      should_publish = true;
    }
  }

  if (should_publish) {
    fuel_consumption_sensor_->publish_state(fuel_consumption_lph_);
    fuel_consumption_last_published_ = fuel_consumption_lph_;
  }
}

void AutotermUART::maybe_save_fuel_(uint32_t now, bool force) {
  if (!fuel_dirty_) return;
  if (!force && (now - last_runtime_save_millis_) < CRC_SAVE_INTERVAL_MS) return;
  if (fuel_pref_.save(&total_fuel_liters_)) {
    fuel_dirty_ = false;
  }
}

// ===================
// Total fuel consumed sensor
// ===================
void AutotermUART::publish_total_fuel_(bool force) {
  if (!total_fuel_sensor_) return;

  bool should_publish = force;
  if (!should_publish) {
    if (std::isnan(total_fuel_last_published_) ||
        std::fabs(total_fuel_liters_ - total_fuel_last_published_) >= 0.01f) {
      should_publish = true;
    }
  }

  if (should_publish) {
    total_fuel_sensor_->publish_state(total_fuel_liters_);
    total_fuel_last_published_ = total_fuel_liters_;
  }

  // Publish daily fuel
  if (daily_fuel_sensor_) {
    bool should_publish_daily = force;
    if (!should_publish_daily) {
      if (std::isnan(daily_fuel_last_published_) ||
          std::fabs(daily_fuel_liters_ - daily_fuel_last_published_) >= 0.01f) {
        should_publish_daily = true;
      }
    }
    if (should_publish_daily) {
      daily_fuel_sensor_->publish_state(daily_fuel_liters_);
      daily_fuel_last_published_ = daily_fuel_liters_;
    }
  }
}

// ===================
// Combustion efficiency tracking
// ===================
void AutotermUART::update_combustion_efficiency_(float heater_temp, float ambient_temp) {
  // Use direct thermocouple if available, otherwise fall back to UART temperature
  float effective_exhaust_temp = heater_temp;
  if (exhaust_temp_direct_sensor_ && std::isfinite(exhaust_temp_direct_sensor_->state) &&
      exhaust_temp_direct_sensor_->state > 0.0f) {
    effective_exhaust_temp = exhaust_temp_direct_sensor_->state;
    exhaust_direct_available_ = true;
  } else {
    exhaust_direct_available_ = false;
  }

  if (!std::isfinite(effective_exhaust_temp) || !std::isfinite(ambient_temp))
    return;

  // Compute exhaust temp derivative (dT/dt in °C/min)
  uint32_t now = millis();
  if (last_exhaust_update_ms_ > 0 && std::isfinite(last_exhaust_temp_c_)) {
    float dt_min = static_cast<float>(now - last_exhaust_update_ms_) / 60000.0f;
    if (dt_min > 0.01f && dt_min < 5.0f) {  // Sanity: 0.01-5 minutes between updates
      exhaust_temp_derivative_ = (effective_exhaust_temp - last_exhaust_temp_c_) / dt_min;
    }
  }
  last_exhaust_update_ms_ = now;
  last_exhaust_temp_c_ = effective_exhaust_temp;
  last_ambient_temp_c_ = ambient_temp;

  // Delta-T across heat exchanger
  delta_t_c_ = heater_temp - ambient_temp;

  // Efficiency estimate: ratio of actual delta-T to maximum possible
  // Max delta-T at full power is roughly 350°C (exhaust at 350°C, ambient at 20°C)
  // Efficiency = actual_delta / max_delta, clamped to 0-100%
  float max_delta = 350.0f;
  combustion_efficiency_pct_ = std::min(100.0f, std::max(0.0f, (delta_t_c_ / max_delta) * 100.0f));

  if (delta_t_sensor_) delta_t_sensor_->publish_state(delta_t_c_);
  if (combustion_efficiency_sensor_) combustion_efficiency_sensor_->publish_state(combustion_efficiency_pct_);

  // Track combustion efficiency trend for predictive maintenance
  if (combustion_efficiency_pct_ > 0.0f) {
    efficiency_sum_ += combustion_efficiency_pct_;
    efficiency_samples_++;

    // Update baseline every 100 samples (~200s at 2s polling)
    if (efficiency_samples_ >= 100) {
      float avg = efficiency_sum_ / efficiency_samples_;
      if (efficiency_baseline_ > 0.0f) {
        efficiency_trend_ = avg - efficiency_baseline_;  // Positive = improving, negative = degrading
        if (efficiency_trend_ < -10.0f) {
          ESP_LOGW("autoterm_uart", "Combustion efficiency degrading: %.1f%% trend (baseline=%.1f%%)",
                   efficiency_trend_, efficiency_baseline_);
        }
      }
      efficiency_baseline_ = avg;
      efficiency_sum_ = 0.0f;
      efficiency_samples_ = 0;
    }
  }

  // Exhaust temperature alerts (scientific recommendations: 200-400°C optimal range)
  static uint32_t last_exhaust_alert_ms = 0;
  if ((now - last_exhaust_alert_ms) > 60000) {  // Check every 60 seconds
    last_exhaust_alert_ms = now;
    if (heater_running_ && effective_exhaust_temp > 50.0f) {
      if (effective_exhaust_temp < 200.0f) {
        ESP_LOGW("autoterm_uart", "EXHAUST LOW: %.0f°C < 200°C — incomplete combustion, soot risk",
                 effective_exhaust_temp);
      } else if (effective_exhaust_temp > 450.0f) {
        ESP_LOGW("autoterm_uart", "EXHAUST HIGH: %.0f°C > 450°C — excess fuel or airflow blocked",
                 effective_exhaust_temp);
      }
    }
  }
}

// ===================
// Ignition time tracking
// ===================
void AutotermUART::track_ignition_time_(uint16_t status_code, uint32_t now) {
  // Start tracking when ignition sequence begins (0x0200-0x0204)
  bool during_ignition = (status_code >= 0x0200 && status_code <= 0x0204);
  bool is_heating = (status_code == STATUS_HEATING);

  if (during_ignition && !ignition_tracking_active_) {
    ignition_start_ms_ = now;
    ignition_tracking_active_ = true;
    ESP_LOGD("autoterm_uart", "Ignition tracking started");
  }

  // Activate burn-out protection when heating starts
  if (is_heating && !burnout_protection_active_) {
    burnout_protection_active_ = true;
    burnout_start_ms_ = now;
    ESP_LOGI("autoterm_uart", "Burn-out protection: active for 4 minutes");
  }

  // Deactivate burn-out protection after 4 minutes
  if (burnout_protection_active_ && (now - burnout_start_ms_) > BURNOUT_PROTECTION_MS) {
    burnout_protection_active_ = false;
    ESP_LOGI("autoterm_uart", "Burn-out protection: expired (4 minutes elapsed)");
  }

  // Stop tracking when heating begins or ignition ends
  if (ignition_tracking_active_ && (is_heating || (!during_ignition && status_code != 0x0200))) {
    uint32_t elapsed = now - ignition_start_ms_;
    float elapsed_s = static_cast<float>(elapsed) / 1000.0f;
    ignition_tracking_active_ = false;

    if (elapsed_s > 1.0f) {  // Only log meaningful ignition times
      last_ignition_time_s_ = elapsed_s;
      if (ignition_time_sensor_) ignition_time_sensor_->publish_state(elapsed_s);

      if (elapsed_s > 420.0f) {  // >7 minutes = problem
        ESP_LOGW("autoterm_uart", "IGNITION TIME LONG: %.0fs (>7min) — check glow plug or fuel delivery", elapsed_s);
      } else {
        ESP_LOGD("autoterm_uart", "Ignition time: %.0fs", elapsed_s);
      }
    }
  }
}

// ===================
// Software watchdog: loop health monitoring
// ===================
void AutotermUART::update_loop_watchdog_(uint32_t now) {
  loop_count_++;

  if (last_loop_watchdog_ms_ > 0) {
    uint32_t interval = now - last_loop_watchdog_ms_;
    if (interval > loop_interval_max_ms_)
      loop_interval_max_ms_ = interval;
    // Running average
    loop_interval_avg_ms_ = (loop_interval_avg_ms_ * loop_interval_count_ + interval) / (loop_interval_count_ + 1);
    loop_interval_count_++;
  }
  last_loop_watchdog_ms_ = now;

  // Log watchdog stats every 5 minutes
  static uint32_t last_watchdog_log = 0;
  if ((now - last_watchdog_log) > 300000) {
    last_watchdog_log = now;
    ESP_LOGI("autoterm_uart", "Watchdog: %lu loops, max_interval=%lums, avg_interval=%lums, free_heap=%u",
             loop_count_, loop_interval_max_ms_, loop_interval_avg_ms_,
             esp_get_free_heap_size());
    // Reset stats for next period
    loop_interval_max_ms_ = 0;
    loop_interval_count_ = 0;
    loop_interval_avg_ms_ = 0;
  }
}

// ===================
// Boot diagnostics
// ===================
void AutotermUART::load_boot_count_() {
  if (global_preferences != nullptr) {
    boot_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("autoterm_boot_count"));
    boot_pref_.load(&boot_count_);
    boot_count_++;
    boot_pref_.save(&boot_count_);
  }
}

const char* AutotermUART::get_reset_reason_str_(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN: return "Unknown";
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Panic/Exception";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Other watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "Unknown reason";
  }
}

void AutotermUART::publish_boot_diagnostics_() {
  if (boot_count_sensor_)
    boot_count_sensor_->publish_state(static_cast<float>(boot_count_));

  if (free_heap_sensor_)
    free_heap_sensor_->publish_state(static_cast<float>(esp_get_free_heap_size()));

  if (reset_reason_sensor_) {
    esp_reset_reason_t reason = esp_reset_reason();
    reset_reason_sensor_->publish_state(get_reset_reason_str_(reason));
  }

  ESP_LOGI("autoterm_uart", "Boot diagnostics: #%u, reset=%s, free_heap=%u",
           boot_count_, get_reset_reason_str_(esp_reset_reason()), esp_get_free_heap_size());
}

// ===================
// Wear curve tracking
// ===================
void AutotermUART::update_wear_score_(float heater_temp, float pump_freq) {
  // Track the ratio of exhaust temp to pump frequency
  // A healthy heater has a consistent ratio; degradation shifts it
  if (!std::isfinite(heater_temp) || !std::isfinite(pump_freq) || pump_freq < 1.0f)
    return;

  float ratio = heater_temp / pump_freq;

  // First sample sets the baseline
  if (!wear_baseline_set_) {
    wear_baseline_ratio_ = ratio;
    wear_baseline_set_ = true;
    ESP_LOGI("autoterm_uart", "Wear baseline established: ratio=%.2f (T_exh=%.0f°C / pump=%.2fHz)",
             ratio, heater_temp, pump_freq);
  }

  // Accumulate for running average
  wear_sum_ratios_ += ratio;
  wear_sample_count_++;

  // Publish wear score every 100 samples (~200s at 2s polling)
  if (wear_sample_count_ >= 100 && wear_score_sensor_) {
    float avg_ratio = wear_sum_ratios_ / wear_sample_count_;
    // Score: 100% = perfect (same as baseline), lower = more wear
    float deviation = std::fabs(avg_ratio - wear_baseline_ratio_) / wear_baseline_ratio_;
    float score = std::max(0.0f, 100.0f - (deviation * 200.0f));  // 50% deviation = 0% score
    wear_score_sensor_->publish_state(score);

    if (score < 70.0f) {
      ESP_LOGW("autoterm_uart", "WEAR ALERT: Score %.0f%% — burner cleaning may be needed (ratio avg=%.2f, baseline=%.2f)",
               score, avg_ratio, wear_baseline_ratio_);
    }

    // Reset for next window
    wear_sum_ratios_ = 0.0f;
    wear_sample_count_ = 0;
  }
}

// ===================
// Fuel economy mode
// ===================
void AutotermUART::update_fuel_economy_savings_(uint32_t now) {
  if (!fuel_economy_active_ || !heater_running_) {
    if (fuel_economy_active_ms_ > 0) {
      fuel_economy_active_ms_ = 0;
    }
    return;
  }

  float current_temp = last_internal_temp_c_;
  if (!std::isfinite(current_temp) || !climate_)
    return;

  float target = climate_->target_temperature;

  // Track time near target (within 1°C) — these are the "efficient" moments
  if (std::isfinite(target) && std::fabs(current_temp - target) < 1.0f) {
    fuel_economy_active_ms_ += 2000;  // ~loop interval
  }

  // Publish savings percentage every minute
  static uint32_t last_savings_publish = 0;
  if ((now - last_savings_publish) > 60000 && fuel_economy_savings_sensor_) {
    float runtime_s = static_cast<float>(runtime_hours_) * 3600.0f;
    if (runtime_s > 60.0f) {
      // Savings = how much time we spent at efficient operation vs total runtime
      // At level 1-3 near target vs level 6-8 at full power = fuel savings
      fuel_economy_savings_pct_ = (static_cast<float>(fuel_economy_active_ms_) / 1000.0f / runtime_s) * 100.0f;
      fuel_economy_savings_pct_ = std::min(50.0f, fuel_economy_savings_pct_);
      fuel_economy_savings_sensor_->publish_state(fuel_economy_savings_pct_);
    }
    last_savings_publish = now;
  }
}

// ===================
// System health monitoring
// ===================
void AutotermUART::update_system_health_(uint32_t now) {
  static uint32_t last_health_check = 0;
  if ((now - last_health_check) < 60000) return;  // Check every 60 seconds
  last_health_check = now;

  // Build health status string
  std::string health;
  int issues = 0;

  // Check CRC error rate
  if (frame_count_ > 100) {
    float error_rate = static_cast<float>(crc_error_count_) / frame_count_ * 100.0f;
    if (error_rate > 5.0f) {
      health += "CRC:" + std::to_string(static_cast<int>(error_rate)) + "% ";
      issues++;
    }
  }

  // Check voltage stability
  if (std::isfinite(voltage_sensor_->state)) {
    float v = voltage_sensor_->state;
    if (v < 11.0f) { health += "VLOW:"; health += std::to_string(v); health += "V "; issues++; }
    else if (v > 14.5f) { health += "VHIGH:"; health += std::to_string(v); health += "V "; issues++; }
  }

  // Check UART connectivity
  if (!heater_connected_) { health += "NO_UART "; issues++; }

  // Check for active errors
  if (last_error_code_ != 0x00) {
    health += "ERR:" + std::to_string(last_error_code_) + " ";
    issues++;
  }

  // Overall status
  if (issues == 0) {
    if (reset_reason_sensor_) reset_reason_sensor_->publish_state("healthy");
  } else {
    ESP_LOGW("autoterm_uart", "System health: %d issues detected — %s", issues, health.c_str());
  }
}

// ===================
// Altitude compensation via GPS (no hardware needed)
// ===================
void AutotermUART::update_altitude_compensation_() {
  if (!altitude_sensor_ || !heater_running_)
    return;

  float altitude = altitude_sensor_->state;
  if (!std::isfinite(altitude) || altitude < -500.0f || altitude > 10000.0f)
    return;

  current_altitude_m_ = altitude;

  // Barometric formula: air density decreases ~12% per 1000m
  // density_factor = (1 - altitude/44330)^5.256 (simplified ISA model)
  // At sea level: 1.0, at 1000m: ~0.89, at 2000m: ~0.78, at 3000m: ~0.69
  float density_factor = std::pow(1.0f - altitude / 44330.0f, 5.256f);
  density_factor = std::max(0.5f, std::min(1.0f, density_factor));  // Clamp to 50-100%

  if (std::fabs(density_factor - air_density_factor_) > 0.02f) {
    air_density_factor_ = density_factor;
    ESP_LOGI("autoterm_uart", "Altitude compensation: %.0fm -> density factor %.2f (max level capped to %u)",
             altitude, density_factor, static_cast<unsigned>(9 * density_factor));
  }
}

// ===================
// Intelligent prediction: temperature patterns
// ===================
void AutotermUART::update_prediction_(float temp, uint32_t now) {
  if (!prediction_active_ || !std::isfinite(temp))
    return;

  // Get current hour from system time
  struct tm timeinfo;
  if (!now_local(&timeinfo))
    return;
  int hour = timeinfo.tm_hour;

  // Initialize on first call
  if (!prediction_initialized_) {
    for (int i = 0; i < 24; i++) {
      prediction_temps_[i] = NAN;
      prediction_counts_[i] = 0;
    }
    load_prediction_data_();
    prediction_initialized_ = true;
  }

  // Update running average for this hour
  if (std::isfinite(prediction_temps_[hour])) {
    // Weighted moving average (new samples have more weight)
    float weight = 0.1f;
    prediction_temps_[hour] = prediction_temps_[hour] * (1.0f - weight) + temp * weight;
  } else {
    prediction_temps_[hour] = temp;
  }
  prediction_counts_[hour]++;

  // Save every 100 samples
  static uint32_t prediction_save_counter = 0;
  prediction_save_counter++;
  if (prediction_save_counter >= 100) {
    prediction_save_counter = 0;
    save_prediction_data_();
  }
}

void AutotermUART::publish_predicted_temp_() {
  if (!prediction_active_ || !predicted_temp_sensor_)
    return;

  struct tm timeinfo;
  if (!now_local(&timeinfo))
    return;
  int hour = timeinfo.tm_hour;

  if (prediction_initialized_ && std::isfinite(prediction_temps_[hour])) {
    predicted_temp_sensor_->publish_state(prediction_temps_[hour]);
  }
}

void AutotermUART::load_prediction_data_() {
  if (global_preferences == nullptr)
    return;

  // Load prediction data (24 floats + 24 uint32_t = 192 bytes)
  auto pred_temp_pref = global_preferences->make_preference<float[24]>(fnv1_hash("autoterm_pred_temps"));
  float loaded_temps[24];
  if (pred_temp_pref.load(loaded_temps)) {
    for (int i = 0; i < 24; i++)
      prediction_temps_[i] = loaded_temps[i];
  }

  auto pred_count_pref = global_preferences->make_preference<uint32_t[24]>(fnv1_hash("autoterm_pred_counts"));
  uint32_t loaded_counts[24];
  if (pred_count_pref.load(loaded_counts)) {
    for (int i = 0; i < 24; i++)
      prediction_counts_[i] = loaded_counts[i];
  }
}

void AutotermUART::save_prediction_data_() {
  if (global_preferences == nullptr)
    return;

  auto pred_temp_pref = global_preferences->make_preference<float[24]>(fnv1_hash("autoterm_pred_temps"));
  pred_temp_pref.save(prediction_temps_);

  auto pred_count_pref = global_preferences->make_preference<uint32_t[24]>(fnv1_hash("autoterm_pred_counts"));
  pred_count_pref.save(prediction_counts_);
}

// ===================
// Light sleep optimization
// ===================
void AutotermUART::evaluate_light_sleep_(uint32_t now) {
  if (!light_sleep_enabled_)
    return;

  // Don't sleep if heater is running or UART is active
  if (heater_running_)
    return;

  // Don't sleep if display was recently active
  if ((now - last_display_activity_) < 30000)
    return;

  // Light sleep is managed by ESP-IDF automatic power management via sdkconfig.
  // When CONFIG_PM_ENABLE is set, the CPU automatically enters light sleep
  // during the RTOS idle task, saving ~50-80mA compared to active mode.
  //
  // UART wake-up is configured via CONFIG_UART_ISR_IN_IRAM and GPIO wake-up.
  // The ESP32 will wake on any UART activity (heater status frames, display commands).

  static uint32_t last_sleep_log = 0;
  static bool sleep_mode_active = false;

  if (!sleep_mode_active) {
    // Configure UART as wake-up source for light sleep
    gpio_num_t uart_rx_gpio = GPIO_NUM_16;  // uart_panel RX
    esp_sleep_enable_gpio_wakeup();
    // Note: ESPHome's UART component handles the actual GPIO configuration.
    // The sdkconfig PM settings handle automatic light sleep entry/exit.

    sleep_mode_active = true;
    ESP_LOGI("autoterm_uart", "Light sleep: power management active, CPU will idle-sleep when heater off");
  }

  if ((now - last_sleep_log) > 300000) {  // Every 5 minutes
    last_sleep_log = now;
    ESP_LOGD("autoterm_uart", "Light sleep: system idle, CPU in low-power mode");
  }
}

// ===================
// Active fuel economy: reduce power when near target
// ===================
void AutotermUART::evaluate_fuel_economy_reactive_(uint32_t now) {
  if (!fuel_economy_reactive_ || !thermostat_active_ || !heater_running_)
    return;

  float current_temp = get_temperature_for_source(thermostat_sensor_source_);
  if (!std::isfinite(current_temp))
    return;

  float target = thermostat_target_c_;
  float error = target - current_temp;

  // If within 0.5°C of target, reduce power
  if (error < 0.5f && error > -0.5f) {
    if (economy_reduced_level_ == 0) {
      // Start reduction: lower level by 1-2 steps
      economy_reduced_level_ = std::max<uint8_t>(1, thermostat_level_ - 2);
      economy_reduction_start_ms_ = now;
      ESP_LOGD("autoterm_uart", "Fuel economy: reducing power from %u to %u (temp=%.1f°C near target)",
               thermostat_level_, economy_reduced_level_, current_temp);
      send_power_mode(false, economy_reduced_level_);
    }
  } else if (error >= 0.5f) {
    // Too far below target, restore normal level
    if (economy_reduced_level_ > 0) {
      ESP_LOGD("autoterm_uart", "Fuel economy: restoring power to %u (temp=%.1f°C below target)",
               thermostat_level_, current_temp);
      send_power_mode(false, thermostat_level_);
      economy_reduced_level_ = 0;
    }
  }
}

// ===================
// Periodic backup of all persistent data
// ===================
void AutotermUART::periodic_backup_(uint32_t now) {
  static uint32_t last_backup = 0;
  if ((now - last_backup) < 600000)  // Every 10 minutes
    return;
  last_backup = now;

  // Redundant save of all critical data
  maybe_save_runtime_hours_(now, true);
  maybe_save_fuel_(now, true);
  save_stats_();
  save_burn_cycle_hours_();
  save_maintenance_counters_();

  // Save prediction data if active
  if (prediction_active_ && prediction_initialized_)
    save_prediction_data_();

  ESP_LOGD("autoterm_uart", "Periodic backup: runtime=%.1fh, fuel=%.2fL, starts=%u",
           runtime_hours_, total_fuel_liters_, total_start_count_);
}

// ===================
// System health summary
// ===================
void AutotermUART::publish_system_health_() {
  // Compile a health summary for logging
  uint32_t now = millis();
  float uptime_h = static_cast<float>(now) / 3600000.0f;

  ESP_LOGI("autoterm_uart",
           "HEALTH: uptime=%.1fh | free_heap=%u | crc_errors=%u/%u (%.1f%%) | "
           "starts=%u | runtime=%.1fh | fuel=%.2fL | emergency=%s | heater=%s",
           uptime_h,
           esp_get_free_heap_size(),
           crc_error_count_, frame_count_,
           frame_count_ > 0 ? (static_cast<float>(crc_error_count_) / frame_count_ * 100.0f) : 0.0f,
           total_start_count_, runtime_hours_, total_fuel_liters_,
           emergency_shutdown_active_ ? "YES" : "no",
           heater_running_ ? "running" : "off");
}

// ===================
// Startup phase monitor
// ===================
void AutotermUART::monitor_startup_phase_(uint16_t status_code, float voltage, float heater_temp, uint32_t now) {
  // Track the ignition sequence phases and log timing
  bool in_ignition = (status_code >= 0x0200 && status_code <= 0x0204);
  bool was_in_ignition = (last_startup_status_ >= 0x0200 && last_startup_status_ <= 0x0204);

  if (in_ignition && !was_in_ignition) {
    // Starting ignition sequence
    startup_phase_start_ms_ = now;
    startup_monitoring_active_ = true;
    ESP_LOGI("autoterm_uart", "STARTUP: Ignition sequence initiated");
  }

  if (startup_monitoring_active_ && in_ignition) {
    uint32_t elapsed = now - startup_phase_start_ms_;

    // Log each phase transition
    if (status_code != last_startup_status_) {
      const char *phase = "unknown";
      switch (status_code) {
        case 0x0200: phase = "preparing"; break;
        case 0x0201: phase = "glow_plug_heating"; break;
        case 0x0202: phase = "ignition_1"; break;
        case 0x0203: phase = "ignition_2"; break;
        case 0x0204: phase = "combustion_chamber_heating"; break;
      }
      ESP_LOGI("autoterm_uart", "STARTUP: Phase -> %s (%.0fs elapsed)", phase, elapsed / 1000.0f);
    }

    // Voltage check during glow plug phase
    if (status_code == 0x0201 && std::isfinite(voltage) && voltage < 11.0f) {
      ESP_LOGW("autoterm_uart", "STARTUP: Low voltage during glow plug phase: %.1fV", voltage);
    }

    // Temperature check: should be rising during ignition
    if (status_code >= 0x0203 && std::isfinite(heater_temp) && heater_temp < 50.0f && elapsed > 30000) {
      ESP_LOGW("autoterm_uart", "STARTUP: Exhaust temp low (%.0f°C) after %.0fs — check fuel delivery",
               heater_temp, elapsed / 1000.0f);
    }
  }

  if (startup_monitoring_active_ && !in_ignition && status_code == STATUS_HEATING) {
    // Ignition complete — heater is now running
    uint32_t total_startup_ms = now - startup_phase_start_ms_;
    float startup_s = total_startup_ms / 1000.0f;
    ESP_LOGI("autoterm_uart", "STARTUP: Complete in %.1fs — heater running", startup_s);

    if (startup_s > 120.0f) {
      ESP_LOGW("autoterm_uart", "STARTUP: Slow startup (%.0fs > 120s) — check glow plug or fuel filter", startup_s);
    }

    startup_monitoring_active_ = false;
  }

  last_startup_status_ = status_code;
}

// ===================
// Maintenance reminders
// ===================
void AutotermUART::check_maintenance_() {
  if (!runtime_loaded_) return;

  // Oil change reminder (configurable threshold)
  bool oil_alert = runtime_hours_ >= maintenance_oil_hrs_;
  if (oil_alert != maintenance_alert_oil_) {
    maintenance_alert_oil_ = oil_alert;
    if (oil_alert)
      ESP_LOGW("autoterm_uart", "MAINTENANCE: Oil change due! (%.1f / %.0f hours)", runtime_hours_, maintenance_oil_hrs_);
  }

  // Filter cleaning reminder: check BOTH time-based AND fuel-volume-based thresholds
  // Fuel volume is more accurate than time because filter loading depends on fuel volume, not runtime
  static constexpr float FILTER_FUEL_VOLUME_LITERS = 50.0f;  // Replace filter every 50 liters
  bool filter_alert_time = runtime_hours_ >= maintenance_filter_hrs_;
  bool filter_alert_volume = total_fuel_liters_ >= FILTER_FUEL_VOLUME_LITERS;
  bool filter_alert = filter_alert_time || filter_alert_volume;
  if (filter_alert != maintenance_alert_filter_) {
    maintenance_alert_filter_ = filter_alert;
    if (filter_alert) {
      if (filter_alert_volume && !filter_alert_time) {
        ESP_LOGW("autoterm_uart", "MAINTENANCE: Filter cleaning due! (%.1fL / %.0fL fuel volume)",
                 total_fuel_liters_, FILTER_FUEL_VOLUME_LITERS);
      } else {
        ESP_LOGW("autoterm_uart", "MAINTENANCE: Filter cleaning due! (%.1f / %.0f hours)", runtime_hours_, maintenance_filter_hrs_);
      }
    }
  }

  // Glow plug replacement reminder (configurable threshold)
  bool glow_alert = runtime_hours_ >= maintenance_glow_hrs_;
  if (glow_alert != maintenance_alert_glow_) {
    maintenance_alert_glow_ = glow_alert;
    if (glow_alert)
      ESP_LOGW("autoterm_uart", "MAINTENANCE: Glow plug replacement due! (%.1f / %.0f hours)", runtime_hours_, maintenance_glow_hrs_);
  }
}

void AutotermUART::publish_maintenance_() {
  if (maintenance_oil_sensor_)
    maintenance_oil_sensor_->publish_state(maintenance_alert_oil_ ? 1.0f : 0.0f);
  if (maintenance_filter_sensor_)
    maintenance_filter_sensor_->publish_state(maintenance_alert_filter_ ? 1.0f : 0.0f);
  if (maintenance_glow_sensor_)
    maintenance_glow_sensor_->publish_state(maintenance_alert_glow_ ? 1.0f : 0.0f);
  publish_maintenance_counters_();
}

// ===================
// Frost protection
// ===================
void AutotermUART::evaluate_frost_protection_() {
  if (!frost_protection_active_) return;
  if (heater_running_) return;  // Already running, no need

  float ext_temp = last_external_temp_c_;
  if (!std::isfinite(ext_temp)) return;

  // === Antifreeze zones with asymmetric hysteresis (from zatakon/esphome-vevor-heater) ===
  // 4 power zones based on external temperature:
  //   Zone 1: ext > 5°C     -> OFF (no heating)
  //   Zone 2: 0°C < ext <= 5°C  -> Level 1-2 (minimal)
  //   Zone 3: -5°C < ext <= 0°C -> Level 3-5 (moderate)
  //   Zone 4: ext <= -5°C    -> Level 6-9 (aggressive)
  //
  // Asymmetric hysteresis: power increases delayed by 0.4°C, decreases immediate

  uint8_t target_level = 0;

  if (ext_temp > 5.0f) {
    target_level = 0;  // Zone 1: OFF
  } else if (ext_temp > 0.0f) {
    target_level = std::max(static_cast<uint8_t>(1),
                            static_cast<uint8_t>((5.0f - ext_temp) / 5.0f * 2.0f + 1.0f));  // 1-2
  } else if (ext_temp > -5.0f) {
    target_level = std::max(static_cast<uint8_t>(3),
                            static_cast<uint8_t>((0.0f - ext_temp) / 5.0f * 2.0f + 3.0f));  // 3-5
  } else {
    target_level = std::max(static_cast<uint8_t>(6),
                            static_cast<uint8_t>((-5.0f - ext_temp) / 10.0f * 3.0f + 6.0f));  // 6-9
  }
  target_level = std::min(target_level, static_cast<uint8_t>(9));

  if (target_level == 0) {
    // Temperature is warm enough — don't start
    return;
  }

  // Asymmetric hysteresis: delay power increases by 0.4°C, decreases are immediate
  static uint8_t last_frost_level = 0;
  static uint32_t last_frost_increase_ms = 0;

  if (target_level > last_frost_level) {
    // Power increase — check if we should delay
    uint32_t now = millis();
    if (last_frost_increase_ms > 0 && (now - last_frost_increase_ms) < 400) {
      return;  // Delay increase by 0.4 seconds (scaled from 0.4°C concept)
    }
    last_frost_increase_ms = now;
  }

  last_frost_level = target_level;
  ESP_LOGW("autoterm_uart", "Frost protection: ext=%.1f°C -> zone level %u", ext_temp, target_level);
  send_power_mode(true, target_level);
  // Do NOT set frost_protection_active_ = false — keep monitoring continuously
  // If heater shuts down later, frost protection will re-engage on next 30s check

  // === Prediction-based pre-heating: start early at low power if temp will drop ===
  if (!prediction_active_ || !prediction_initialized_) return;

  struct tm timeinfo;
  if (!now_local(&timeinfo)) return;
  int current_hour = timeinfo.tm_hour;

  // Check next 2 hours for predicted temperature drop
  for (int h = 1; h <= 2; h++) {
    int check_hour = (current_hour + h) % 24;
    if (!std::isfinite(prediction_temps_[check_hour])) continue;

    float predicted_temp = prediction_temps_[check_hour];
    if (predicted_temp < frost_protection_temp_c_ && ext_temp > frost_protection_temp_c_) {
      // Temperature will drop below threshold in next 2 hours — pre-heat now at low power
      ESP_LOGI("autoterm_uart", "Pre-heating: predicted T=%.1f°C at hour %d (below %.1f°C), starting at level 1",
               predicted_temp, check_hour, frost_protection_temp_c_);
      send_power_mode(true, 1);
      frost_protection_active_ = false;
      return;
    }
  }
}

// ===================
// Safety: Emergency shutdown
// ===================
void AutotermUART::emergency_stop_(const char *reason) {
  if (emergency_shutdown_active_) return;  // Already in emergency
  emergency_shutdown_active_ = true;
  ESP_LOGE("autoterm_uart", "EMERGENCY STOP: %s — shutting down heater!", reason);
  send_standby();
  // Also disable thermostat to prevent auto-restart
  disable_thermostat_mode();
  // Publish error state
  if (error_text_sensor_) error_text_sensor_->publish_state(reason);
}

// Recovery from emergency shutdown
// Can be triggered by:
// 1. User pressing unlock button (clears error 37 + emergency state)
// 2. Automatic after 5 minutes if conditions are safe
void AutotermUART::check_emergency_recovery_(uint32_t now) {
  if (!emergency_shutdown_active_)
    return;

  // Record when emergency started
  if (emergency_start_ms_ == 0) {
    emergency_start_ms_ = now;
    return;
  }

  // Auto-recovery after 5 minutes if conditions are safe
  if ((now - emergency_start_ms_) < 300000) return;  // 5 minutes

  // Check if conditions are safe to recover
  bool safe = true;
  if (std::isfinite(last_heater_temp_c_) && last_heater_temp_c_ > 350.0f) safe = false;
  if (std::isfinite(voltage_sensor_->state) && voltage_sensor_->state < 10.0f) safe = false;

  if (safe) {
    ESP_LOGI("autoterm_uart", "Emergency recovery: conditions safe, clearing emergency state");
    emergency_shutdown_active_ = false;
    emergency_start_ms_ = 0;
    if (error_text_sensor_) error_text_sensor_->publish_state("recovered");
  }
}

void AutotermUART::check_emergency_shutdown_(float heater_temp, float voltage, uint16_t status_code) {
  // 1. Over-temperature protection (immediate, no confirmation needed)
  // ALWAYS active, even during burn-out protection
  if (std::isfinite(heater_temp) && heater_temp > SAFETY_MAX_HEATER_TEMP_C) {
    emergency_stop_("Heater temperature critical");
    return;
  }

  // 2. During burn-out protection (first 4 min after ignition), only critical over-temp triggers shutdown
  // Voltage dips and flameout are suppressed to prevent damage from incomplete combustion
  if (burnout_protection_active_) {
    static uint32_t last_burnout_log = 0;
    if ((millis() - last_burnout_log) > 60000) {
      last_burnout_log = millis();
      ESP_LOGD("autoterm_uart", "Burn-out protection: suppressing non-critical shutdown");
    }
    return;
  }

  // 3. Critical low voltage during operation (requires 2 consecutive readings)
  // Transient voltage dips during glow plug startup are normal
  if (std::isfinite(voltage) && voltage < 9.0f && is_heater_active_status_(status_code)) {
    voltage_dip_confirm_count_++;
    if (voltage_dip_confirm_count_ >= 2) {
      emergency_stop_("Battery voltage critically low (confirmed)");
      voltage_dip_confirm_count_ = 0;
    } else {
      ESP_LOGW("autoterm_uart", "Voltage dip detected: %.1fV (confirming %u/2)", voltage,
               static_cast<unsigned>(voltage_dip_confirm_count_));
    }
  } else {
    voltage_dip_confirm_count_ = 0;  // Reset if voltage recovers
  }
}

// ===================
// Safety: Flameout detection
// ===================
void AutotermUART::check_flameout_(float heater_temp, float pump_freq, uint32_t now) {
  bool pump_active = pump_freq > 0.5f;  // Pump running above minimum threshold

  if (pump_active) {
    if (!pump_was_active_) {
      // Pump just started — begin tracking
      pump_active_since_ms_ = now;
      pump_was_active_ = true;
      ESP_LOGD("autoterm_uart", "Flameout monitor: pump active, tracking started");
    }

    // Check if heater temp is too low for too long
    uint32_t pump_runtime = now - pump_active_since_ms_;
    if (pump_runtime > SAFETY_FLAMEOUT_TIMEOUT_MS &&
        std::isfinite(heater_temp) && heater_temp < SAFETY_FLAMEOUT_MIN_TEMP_C) {
      flameout_confirm_count_++;
      if (flameout_confirm_count_ >= 2) {
        emergency_stop_("Flameout detected: pump active but exhaust temp too low (confirmed)");
        flameout_confirm_count_ = 0;
      } else {
        ESP_LOGW("autoterm_uart", "Flameout: low temp detected, confirming (attempt %u/2)",
                 static_cast<unsigned>(flameout_confirm_count_));
      }
    } else {
      flameout_confirm_count_ = 0;  // Reset if temp recovers
    }
  } else {
    // Pump stopped — reset tracking
    if (pump_was_active_) {
      ESP_LOGD("autoterm_uart", "Flameout monitor: pump stopped, resetting timer");
    }
    pump_was_active_ = false;
    pump_active_since_ms_ = 0;
  }
}

// ===================
// Safety: Startup voltage check
// ===================
void AutotermUART::check_startup_voltage_(float voltage, uint16_t status_code) {
  // Only check during ignition sequence (0x0200-0x0204)
  bool during_ignition = (status_code >= 0x0200 && status_code <= 0x0204);
  if (!during_ignition) {
    startup_voltage_ok_ = true;  // Reset after ignition phase
    return;
  }

  if (std::isfinite(voltage) && voltage < SAFETY_MIN_STARTUP_VOLTAGE_V) {
    if (startup_voltage_ok_) {
      startup_voltage_ok_ = false;
      ESP_LOGE("autoterm_uart",
               "STARTUP VOLTAGE LOW: %.1fV < %.1fV — glow plug may fail, start may abort",
               voltage, SAFETY_MIN_STARTUP_VOLTAGE_V);
    }
  }
}

// ===================
// CO sensor check (SAVES LIVES)
// ===================
void AutotermUART::check_co_level_() {
  if (!co_sensor_ || !heater_running_)
    return;

  float co_ppm = co_sensor_->state;
  if (!std::isfinite(co_ppm) || co_ppm < 0.0f)
    return;

  last_co_ppm_ = co_ppm;

  if (co_ppm >= CO_DANGER_PPM) {
    co_confirm_count_++;
    if (co_confirm_count_ >= CO_CONFIRM_REQUIRED) {
      emergency_stop_("CO level critical: %.1f ppm >= %.1f ppm — LEAK DETECTED", co_ppm, CO_DANGER_PPM);
      co_confirm_count_ = 0;
    } else {
      ESP_LOGW("autoterm_uart", "CO WARNING: %.1f ppm (confirming %u/%u)",
               co_ppm, static_cast<unsigned>(co_confirm_count_), static_cast<unsigned>(CO_CONFIRM_REQUIRED));
    }
  } else {
    co_confirm_count_ = 0;  // Reset if CO drops below threshold
  }
}

// ===================
// Statistics
// ===================
void AutotermUART::load_stats_() {
  if (global_preferences != nullptr) {
    stats_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("autoterm_start_count"));
    stats_pref_.load(&total_start_count_);
  }
}

void AutotermUART::save_stats_() {
  if (stats_dirty_ && global_preferences != nullptr) {
    stats_pref_.save(&total_start_count_);
    stats_dirty_ = false;
  }
}

void AutotermUART::increment_start_count_() {
  total_start_count_++;
  stats_dirty_ = true;
}

// ===================
// Frame processing
// ===================

void AutotermUART::process_frame_(std::vector<uint8_t> frame, uart::UARTComponent *dst, const char *tag, bool from_display) {
  if (frame.empty())
    return;

  // Validate frame structure first
  if (!validate_frame_structure_(frame)) {
    ESP_LOGW("autoterm_uart", "[%s] Invalid frame structure, discarding", tag);
    return;
  }

  bool valid = validate_crc(frame);
  std::vector<uint8_t> outgoing = frame;

  // Track CRC errors with rate monitoring and resync
  frame_count_++;

  if (!valid) {
    crc_error_count_++;
    float error_rate = static_cast<float>(crc_error_count_) / frame_count_ * 100.0f;

    ESP_LOGW("autoterm_uart", "[%s] CRC error (total: %u/%u = %.2f%%) — frame discarded",
             tag, crc_error_count_, frame_count_, error_rate);

    // Alert if error rate > 5%
    if (error_rate > 5.0f && frame_count_ > 100) {
      ESP_LOGE("autoterm_uart", "CRC error rate critical: %.2f%% - check UART connections!", error_rate);
    }

    // Do NOT forward invalid frames — heater/display will handle missing frame via timeout
    auto &buffer = from_display ? display_to_heater_buffer_ : heater_to_display_buffer_;
    resync_uart_buffer_(buffer);
    return;
  }

  // === Only forward VALID frames ===
  if (valid && from_display) {
    if (is_panel_temperature_frame_(outgoing) && should_override_panel_temperature_()) {
      if (outgoing.size() > 5) {
        uint8_t original_byte = outgoing[5];
        uint8_t override_byte = compute_override_temperature_byte_();
        if (override_byte != original_byte) {
          outgoing[5] = override_byte;
          update_crc_(outgoing);
          ESP_LOGD("autoterm_uart", "Panel temp override active: %u -> %u (source %.1f°C)",
                   static_cast<unsigned>(original_byte),
                   static_cast<unsigned>(override_byte),
                   panel_temp_override_value_c_);
        }
      }
    }
    apply_temp_source_override_(outgoing);
  }

  if (dst != nullptr && !outgoing.empty()) {
    dst->write_array(outgoing.data(), outgoing.size());
    dst->flush();
  }

  if (is_panel_temperature_frame_(outgoing))
    handle_panel_temperature_frame_(outgoing);
  log_frame(tag, outgoing);
  parse_status(outgoing);
  parse_settings(outgoing, from_display);

  // Parse extended protocol responses (version, history, diagnostic)
  if (outgoing[1] == DEVICE_HEATER) {
    if (outgoing[4] == FUNC_VERSION) parse_version_(outgoing);
    if (outgoing[4] == FUNC_REPORT) parse_history_(outgoing);
  }
  // Diagnostic telemetry uses device ID 0x02, command 0x01
  if (outgoing[1] == DEVICE_DIAG && outgoing[4] == 0x01) {
    parse_diagnostic_(outgoing);
  }
}

void AutotermUART::publish_temp_source_select_(uint8_t source) {
  if (temp_source_select_ != nullptr) {
    uint8_t clamped = clamp_temp_source_(source);
    temp_source_select_->publish_for_source(clamped);
  }
}

uint8_t AutotermUART::clamp_temp_source_(uint8_t source) const {
  if (source < 1)
    return 1;
  if (source > 4)
    return 4;
  return source;
}

bool AutotermUART::should_force_temp_source_() const {
  return manual_temp_source_active_ && manual_temp_source_value_ >= 1 && manual_temp_source_value_ <= 4;
}

uint8_t AutotermUART::map_source_to_heater_(uint8_t source) const {
  switch (clamp_temp_source_(source)) {
    case 1:
      return 0x01;
    case 2:
      return 0x02;
    case 3:
      return 0x03;
    case 4:
      return 0x02;  // Home Assistant reports to the heater as a panel sensor
    default:
      return 0x01;
  }
}

void AutotermUART::apply_temp_source_override_(std::vector<uint8_t> &frame) {
  if (!should_force_temp_source_())
    return;
  if (frame.size() < 7)
    return;
  if (frame[0] != FRAME_HEADER || frame[1] != DEVICE_DISPLAY)
    return;
  uint8_t command = frame[4];
  if (command != FUNC_POWER_START && command != FUNC_SETTINGS)
    return;
  size_t payload_index = 5;
  if (frame.size() <= payload_index + 2)
    return;
  uint8_t desired = map_source_to_heater_(manual_temp_source_value_);
  uint8_t current = frame[payload_index + 2];
  if (current == desired)
    return;
  frame[payload_index + 2] = desired;
  update_crc_(frame);
  ESP_LOGD("autoterm_uart", "Temperature source override active: %u -> %u",
           static_cast<unsigned>(current), static_cast<unsigned>(desired));
}

bool AutotermUART::should_override_panel_temperature_() const {
  if (!std::isfinite(panel_temp_override_value_c_))
    return false;
  uint8_t source = 0;
  if (manual_temp_source_active_ && manual_temp_source_value_ >= 1 && manual_temp_source_value_ <= 4)
    source = manual_temp_source_value_;
  else if (settings_valid_)
    source = settings_.temperature_source;
  if (source != 4)
    return false;
  return true;
}

uint8_t AutotermUART::compute_override_temperature_byte_() const {
  float value = panel_temp_override_value_c_;
  if (!std::isfinite(value))
    value = 0.0f;
  if (value < 0.0f)
    value = 0.0f;
  if (value > 99.0f)
    value = 99.0f;
  return static_cast<uint8_t>(std::round(value));
}

void AutotermUART::update_crc_(std::vector<uint8_t> &frame) {
  if (frame.size() < 3)
    return;
  uint16_t crc = crc16_modbus_(frame.data(), frame.size() - 2);
  frame[frame.size() - 2] = (crc >> 8) & 0xFF;
  frame[frame.size() - 1] = crc & 0xFF;
}

// ===================
// Existing methods
// ===================

void AutotermUART::parse_status(const std::vector<uint8_t> &data) {
  if (data.size() < 24) return;
  if (data[1] != DEVICE_HEATER || data[4] != FUNC_STATUS) return;

  const uint8_t *p = &data[5];
  uint8_t s_hi = p[0];
  uint8_t s_lo = p[1];
  uint16_t status_code = (static_cast<uint16_t>(s_hi) << 8) | s_lo;
  uint8_t fan_set_raw = p[11];
  uint8_t fan_actual_raw = p[12];
  uint8_t pump_raw = p[14];
  float status_val = s_hi + (s_lo / 10.0f);
  float internal_temp = (p[3] > 127 ? p[3] - 255 : p[3]);
  float external_temp = (p[4] > 127 ? p[4] - 255 : p[4]);
  uint8_t error_code = p[2];  // Error code (byte 2 of status response)
  float voltage = p[6] / 10.0f;
  uint16_t heater_temp_raw = (static_cast<uint16_t>(p[7]) << 8) | p[8];
  float heater_temp = NAN;
  if (heater_temp_raw != 0xFFFF)
    heater_temp = (static_cast<float>(heater_temp_raw) - 0x100)/2;
  float fan_set_rpm = fan_set_raw * 60.0f;
  float fan_actual_rpm = fan_actual_raw * 60.0f;
  float pump_freq = pump_raw / 100.0f;

  const char *status_txt = "Unknown";
  switch (status_code) {
    case STATUS_STANDBY:
      status_txt = "Standby";
      break;
    case 0x0100:
      status_txt = "Flame sensor cools";
      break;
    case STATUS_VENTILATION:
      status_txt = "Ventilation";
      break;
   case 0x0200:
      status_txt = "Heating is being prepared";
      break;
    case 0x0201:
      status_txt = "Glow plug heats";
      break;
    case 0x0202:
      status_txt = "Ignition 1";
      break;
    case 0x0203:
      status_txt = "Ignition 2";
      break;
    case 0x0204:
      status_txt = "Combustion chamber heating";
      break;
    case STATUS_HEATING:
      status_txt = "Heating";
      break;
    case STATUS_FANS_ONLY:
      status_txt = "Fans only";
      break;
    case STATUS_COOLING:
      status_txt = "Cools down";
      break;
    case STATUS_IDLE_VENT:
      status_txt = "Idle ventilation";
      break;
    case STATUS_SHUTDOWN:
      status_txt = "Shutdown";
      break;
    default:
      // If unknown status, add HEX values to text output
      static char unknown_buf[32];
      snprintf(unknown_buf, sizeof(unknown_buf), "Unknown (0x%02X%02X)", s_hi, s_lo);
      status_txt = unknown_buf;
      break;
  }

  ESP_LOGD("autoterm_uart",
           "Status: %s (0x%02X%02X) | U=%.1fV | Heater %.0f°C | Fan %.0f/%.0f rpm | Pump %.2f Hz",
           status_txt, s_hi, s_lo, voltage, heater_temp, fan_actual_rpm, fan_set_rpm, pump_freq);

  set_heater_running_state_(is_heater_active_status_(status_code));

  // Cache values for safety checks
  last_heater_temp_c_ = heater_temp;
  last_pump_freq_c_ = pump_freq;

  // Safety: check emergency conditions
  check_emergency_shutdown_(heater_temp, voltage, status_code);
  check_startup_voltage_(voltage, status_code);

  if (internal_temp_sensor_) internal_temp_sensor_->publish_state(internal_temp);
  if (external_temp_sensor_) external_temp_sensor_->publish_state(external_temp);
  if (heater_temp_sensor_) heater_temp_sensor_->publish_state(heater_temp);

  last_internal_temp_c_ = internal_temp;
  // External temperature with fallback cache
  if (std::isfinite(external_temp) && external_temp != 127.0f) {
    last_external_temp_c_ = external_temp;
    last_external_temp_cached_ = external_temp;
    last_external_temp_update_ms_ = millis();
  } else if (std::isfinite(last_external_temp_cached_) &&
             (millis() - last_external_temp_update_ms_) < 300000) {
    // Use cached value for up to 5 minutes if sensor reports disconnected
    last_external_temp_c_ = last_external_temp_cached_;
  }
  handle_thermostat_status_update_(status_code);
  if (thermostat_active_ && !thermostat_waiting_for_idle_)
    evaluate_thermostat_control_(true);

  if (voltage_sensor_) voltage_sensor_->publish_state(voltage);
  if (status_sensor_) status_sensor_->publish_state(status_val);
  if (status_text_sensor_) status_text_sensor_->publish_state(status_txt);
  if (fan_speed_set_sensor_) fan_speed_set_sensor_->publish_state(fan_set_rpm);
  if (fan_speed_actual_sensor_) fan_speed_actual_sensor_->publish_state(fan_actual_rpm);
  if (pump_frequency_sensor_) pump_frequency_sensor_->publish_state(pump_freq);

  // Publish error code from status byte 2
  if (error_code != last_error_code_) {
    last_error_code_ = error_code;
    if (error_code_sensor_) error_code_sensor_->publish_state(error_code);
    if (error_text_sensor_) error_text_sensor_->publish_state(error_code_to_text_(error_code));
    if (error_code != 0x00) {
      ESP_LOGW("autoterm_uart", "Error code: %s (0x%02X)", error_code_to_text_(error_code), error_code);
    }
  }

  // Update fuel consumption from pump frequency
  update_fuel_consumption_(pump_freq);
  publish_fuel_consumption_();
  publish_total_fuel_();

  // Update combustion efficiency tracking
  update_combustion_efficiency_(heater_temp, external_temp);

  // Update wear curve tracking
  update_wear_score_(heater_temp, pump_freq);

  // Update intelligent prediction
  update_prediction_(internal_temp, millis());
  publish_predicted_temp_();

  // Monitor startup phases
  monitor_startup_phase_(status_code, voltage, heater_temp, millis());

  // Track ignition time
  track_ignition_time_(status_code, millis());

  if (climate_) climate_->handle_status_update(status_code, internal_temp);
}

void AutotermUART::parse_settings(const std::vector<uint8_t> &data, bool from_display) {
  if (data.size() < 13) return;
  if (data.size() >= 5 && data[1] == DEVICE_HEATER && data[4] == FUNC_SETTINGS) {
    const uint8_t *p = &data[5];
    uint8_t use_work_time = p[0];
    uint8_t work_time = p[1];
    uint8_t temp_source = p[2];
    uint8_t set_temp = p[3];
    uint8_t wait_mode = p[4];
    uint8_t power_level = p[5];

    ESP_LOGD("autoterm_uart",
             "Settings: use_work_time=%d work_time=%d temp_src=%d set_temp=%d wait_mode=%d level=%d",
             use_work_time, work_time, temp_source, set_temp, wait_mode, power_level);
    Settings s{};
    s.use_work_time = use_work_time;
    s.work_time = work_time;
    s.temperature_source = temp_source;
    s.set_temperature = set_temp;
    s.wait_mode = wait_mode;
    s.power_level = power_level;
    settings_ = s;
    settings_valid_ = true;
    apply_temp_source_from_settings(s.temperature_source);
    if (climate_) climate_->handle_settings_update(settings_, from_display);
  }
}

void AutotermUART::send_fan_mode(bool on, int level) {
  if (!on) {
    send_standby();
    return;
  }
  int clamped = std::max(0, std::min(level, 9));
  send_fan_only(static_cast<uint8_t>(clamped));
}

bool AutotermUART::is_panel_temperature_frame_(const std::vector<uint8_t> &frame) const {
  if (frame.size() < 8)
    return false;
  if (frame[0] != FRAME_HEADER)
    return false;
  if (frame[1] != DEVICE_DISPLAY && frame[1] != DEVICE_HEATER)
    return false;
  if (frame[2] != 0x01)
    return false;
  if (frame[3] != 0x00)
    return false;
  if (frame[4] != FUNC_PANEL_TEMP)
    return false;
  return true;
}

void AutotermUART::handle_panel_temperature_frame_(const std::vector<uint8_t> &frame) {
  if (frame.size() < 6)
    return;

  uint8_t raw = frame[5];
  float temperature_c = static_cast<float>(raw);
  panel_temp_last_value_c_ = temperature_c;

  if (panel_temp_sensor_ != nullptr)
    panel_temp_sensor_->publish_state(temperature_c);
}

uint16_t AutotermUART::crc16_modbus_(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t pos = 0; pos < length; pos++) {
    crc ^= data[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc >>= 1;
    }
  }
  return crc;
}

uint16_t AutotermUART::append_crc_(std::vector<uint8_t> &frame) {
  uint16_t crc = crc16_modbus_(frame.data(), frame.size());
  frame.push_back((crc >> 8) & 0xFF);
  frame.push_back(crc & 0xFF);
  return crc;
}

bool AutotermUART::send_command_(uint8_t command, const std::vector<uint8_t> &payload, const char *log_label) {
  if (!uart_heater_) {
    ESP_LOGW("autoterm_uart", "UART heater not configured, skipping command 0x%02X", command);
    return false;
  }

  uint32_t now = millis();
  if (!check_command_rate_limit_(now))
    return false;

  // Minimum run time check: prevent rapid restart cycles (extends glow plug and pump life)
  // Only applies to start commands (0x01 = power start, 0x23 = fan only)
  if (!heater_running_ && heater_stopped_ms_ > 0 &&
      (command == FUNC_POWER_START || command == FUNC_FAN_ONLY)) {
    uint32_t time_since_stop = now - heater_stopped_ms_;
    if (time_since_stop < MIN_RUN_TIME_MS) {
      ESP_LOGW("autoterm_uart", "Min run time: refusing start (%.0fs since last stop, min 600s)",
               time_since_stop / 1000.0f);
      return false;
    }
  }

  std::vector<uint8_t> frame;
  frame.reserve(5 + payload.size() + 2);
  frame.push_back(FRAME_HEADER);
  frame.push_back(DEVICE_DISPLAY);
  frame.push_back(static_cast<uint8_t>(payload.size()));
  frame.push_back(0x00);
  frame.push_back(command);
  frame.insert(frame.end(), payload.begin(), payload.end());

  uint16_t crc = append_crc_(frame);

  uart_heater_->write_array(frame);
  uart_heater_->flush();

  last_command_millis_ = now;

  // Use stack-allocated buffer for hex logging (avoids heap allocation)
  char payload_hex[64];
  payload_hex[0] = '\0';
  size_t pos = 0;
  for (auto byte : payload) {
    if (pos + 4 < sizeof(payload_hex)) {
      pos += snprintf(payload_hex + pos, sizeof(payload_hex) - pos, "%02X ", byte);
    }
  }
  if (pos > 0) payload_hex[pos - 1] = '\0';  // Remove trailing space

  ESP_LOGD("autoterm_uart", "Sent %s (cmd=0x%02X len=%u payload=[%s] crc=%04X)",
           log_label != nullptr ? log_label : "frame",
           command, static_cast<unsigned>(payload.size()), payload_hex, crc);
  return true;
}

void AutotermUART::send_standby() {
  send_command_(FUNC_STANDBY, {}, "mode.standby");
  // Activate shutdown monitoring to track purge fan sequence
  if (!shutdown_monitoring_active_) {
    shutdown_monitoring_active_ = true;
    shutdown_start_ms_ = millis();
    ESP_LOGI("autoterm_uart", "Shutdown: monitoring purge fan sequence");
  }
}

void AutotermUART::send_power_mode(bool start, uint8_t level) {
  uint8_t clamped_level = std::min<uint8_t>(level, 9);
  // Night mode: limit power level for quieter operation
  if (night_mode_active_ && clamped_level > night_mode_max_level_) {
    ESP_LOGD("autoterm_uart", "Night mode: limiting power level from %u to %u",
             static_cast<unsigned>(clamped_level), static_cast<unsigned>(night_mode_max_level_));
    clamped_level = night_mode_max_level_;
  }
  std::vector<uint8_t> payload{0xFF, 0xFF, 0x04, 0xFF, 0x02, clamped_level};
  send_command_(start ? FUNC_POWER_START : FUNC_SETTINGS, payload, start ? "mode.power_mode.start" : "mode.power_mode.set");
  if (start) increment_start_count_();
}

void AutotermUART::send_temperature_hold_mode(bool start, uint8_t temp_sensor, uint8_t set_temp) {
  uint8_t sensor = map_source_to_heater_(temp_sensor);
  uint8_t temp_byte = std::min<uint8_t>(set_temp, 30);
  std::vector<uint8_t> payload{0xFF, 0xFF, sensor, temp_byte, 0x02, 0xFF};
  send_command_(start ? FUNC_POWER_START : FUNC_SETTINGS, payload, start ? "mode.heating.start" : "mode.heating.set");
}

void AutotermUART::send_temperature_to_fan_mode(bool start, uint8_t temp_sensor, uint8_t set_temp) {
  uint8_t sensor = map_source_to_heater_(temp_sensor);
  uint8_t temp_byte = std::min<uint8_t>(set_temp, 30);
  std::vector<uint8_t> payload{0xFF, 0xFF, sensor, temp_byte, 0x01, 0xFF};
  send_command_(start ? FUNC_POWER_START : FUNC_SETTINGS, payload, start ? "mode.heating_ventilation.start" : "mode.heating_ventilation.set");
}

void AutotermUART::send_fan_only(uint8_t level) {
  uint8_t clamped_level = std::min<uint8_t>(level, 9);
  // Night mode: limit fan speed for quieter operation
  if (night_mode_active_ && clamped_level > night_mode_max_level_) {
    ESP_LOGD("autoterm_uart", "Night mode fan: limiting from %u to %u",
             static_cast<unsigned>(clamped_level), static_cast<unsigned>(night_mode_max_level_));
    clamped_level = night_mode_max_level_;
  }
  std::vector<uint8_t> payload{0xFF, 0xFF, clamped_level, 0xFF};
  send_command_(FUNC_FAN_ONLY, payload, "mode.fan_only");
}

// ===================
// Extended Protocol Commands
// ===================

void AutotermUART::send_diagnostic_mode_(bool enable) {
  std::vector<uint8_t> payload{enable ? 0x01 : 0x00};
  send_command_(FUNC_DIAGNOSTIC, payload, enable ? "diagnostic.enable" : "diagnostic.disable");
  ESP_LOGI("autoterm_uart", "Diagnostic mode %s", enable ? "enabled" : "disabled");
}

void AutotermUART::send_unlock_() {
  send_command_(FUNC_UNLOCK, {}, "unlock");
  ESP_LOGW("autoterm_uart", "Sent unlock command (clear error 37)");
}

void AutotermUART::send_report_request_() {
  send_command_(FUNC_REPORT, {}, "request.report");
}

void AutotermUART::send_version_request_() {
  send_command_(FUNC_VERSION, {}, "request.version");
}

void AutotermUART::send_prime_pump_(uint8_t frequency) {
  std::vector<uint8_t> payload{frequency};
  send_command_(FUNC_PRIME, payload, "prime.pump");
  ESP_LOGI("autoterm_uart", "Prime pump at %u Hz", static_cast<unsigned>(frequency));
}

void AutotermUART::send_handshake_() {
  send_command_(FUNC_HANDSHAKE, {}, "handshake");
}

void AutotermUART::parse_diagnostic_(const std::vector<uint8_t> &data) {
  // Diagnostic telemetry: device=0x02, cmd=0x01, payload=72 bytes
  if (data.size() < 77) return;  // 5 header + 72 payload + 2 CRC
  if (data[1] != DEVICE_DIAG || data[4] != 0x01) return;

  const uint8_t *p = &data[5];

  // Bytes 0-1: Major/Minor state
  uint8_t major_state = p[0];
  uint8_t minor_state = p[1];

  // Bytes 2-4: Total cycle time (24-bit big endian)
  uint32_t cycle_time = (static_cast<uint32_t>(p[2]) << 16) |
                        (static_cast<uint32_t>(p[3]) << 8) | p[4];

  // Bytes 11-12: Fan target/actual (Hz)
  float fan_target = p[11] * 60.0f;  // Convert to RPM
  float fan_actual = p[12] * 60.0f;

  // Bytes 13-14: Glow plug target (PWM duty, 16-bit big endian)
  uint16_t glow_target = (static_cast<uint16_t>(p[13]) << 8) | p[14];
  // Bytes 15-16: Glow plug actual
  uint16_t glow_actual = (static_cast<uint16_t>(p[15]) << 8) | p[16];

  // Byte 17: Fuel pump freq (value / 100 = Hz)
  float pump_freq = p[17] / 100.0f;

  // Bytes 18-19: Chamber temp (Kelvin, big endian)
  uint16_t chamber_raw = (static_cast<uint16_t>(p[18]) << 8) | p[19];
  float chamber_temp_c = chamber_raw - 273.15f;

  // Bytes 20-21: Flame temp (Kelvin, big endian)
  uint16_t flame_raw = (static_cast<uint16_t>(p[20]) << 8) | p[21];
  float flame_temp_c = flame_raw - 273.15f;

  // Byte 24: External sensor (signed, °C)
  float ext_temp = static_cast<int8_t>(p[24]);

  // Byte 25: Board temp (signed, °C)
  float board_temp = static_cast<int8_t>(p[25]);

  // Bytes 26-27: Supply voltage (big endian, / 10)
  uint16_t voltage_raw = (static_cast<uint16_t>(p[26]) << 8) | p[27];
  float voltage = voltage_raw / 10.0f;

  // Byte 28: Fault code
  uint8_t fault_code = p[28];

  ESP_LOGD("autoterm_uart",
           "Diagnostic: state=%u.%u cycle=%lus fan=%.0f/%.0f rpm glow=%u/%u pump=%.2fHz "
           "chamber=%.1f°C flame=%.1f°C ext=%.1f°C board=%.1f°C U=%.1fV fault=%u",
           major_state, minor_state, cycle_time, fan_target, fan_actual,
           glow_target, glow_actual, pump_freq,
           chamber_temp_c, flame_temp_c, ext_temp, board_temp, voltage, fault_code);

  // Publish glow plug current (as PWM duty cycle percentage)
  if (glow_plug_current_sensor_) {
    float glow_pct = (glow_actual / 65535.0f) * 100.0f;
    glow_plug_current_sensor_->publish_state(glow_pct);
  }

  // Publish chamber temp
  if (chamber_temp_sensor_) chamber_temp_sensor_->publish_state(chamber_temp_c);

  // Publish board temp
  if (board_temp_sensor_) board_temp_sensor_->publish_state(board_temp);

  // Publish fault code
  if (error_code_sensor_) error_code_sensor_->publish_state(fault_code);
  if (error_text_sensor_) error_text_sensor_->publish_state(error_code_to_text_(fault_code));
}

void AutotermUART::parse_version_(const std::vector<uint8_t> &data) {
  // Version response: 5-byte payload: Major.Minor.Patch.Build.Bootloader
  if (data.size() < 12) return;  // 5 header + 5 payload + 2 CRC
  if (data[1] != DEVICE_HEATER || data[4] != FUNC_VERSION) return;

  const uint8_t *p = &data[5];
  static char version_buf[32];
  snprintf(version_buf, sizeof(version_buf), "%u.%u.%u.%u (boot:%u)",
           p[0], p[1], p[2], p[3], p[4]);

  ESP_LOGI("autoterm_uart", "Firmware version: %s", version_buf);
  if (firmware_version_sensor_) firmware_version_sensor_->publish_state(version_buf);
}

void AutotermUART::parse_history_(const std::vector<uint8_t> &data) {
  // History response: 7 or 9 bytes
  if (data.size() < 12) return;  // 5 header + 7 payload + 2 CRC (minimum)
  if (data[1] != DEVICE_HEATER || data[4] != FUNC_REPORT) return;

  const uint8_t *p = &data[5];
  history_total_hours_ = (static_cast<uint16_t>(p[0]) << 8) | p[1];
  history_total_starts_ = (static_cast<uint16_t>(p[2]) << 8) | p[3];
  history_errors_[0] = p[4];
  history_errors_[1] = p[5];
  history_errors_[2] = p[6];

  ESP_LOGI("autoterm_uart", "History: %u hours, %u starts, errors=[%u,%u,%u]",
           history_total_hours_, history_total_starts_,
           history_errors_[0], history_errors_[1], history_errors_[2]);

  if (total_starts_sensor_) total_starts_sensor_->publish_state(history_total_starts_);

  // Build error log text
  if (error_log_sensor_) {
    static char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "%u h | %u starts | Err: %s, %s, %s",
             history_total_hours_, history_total_starts_,
             error_code_to_text_(history_errors_[0]),
             error_code_to_text_(history_errors_[1]),
             error_code_to_text_(history_errors_[2]));
    error_log_sensor_->publish_state(log_buf);
  }
}

const char *AutotermUART::error_code_to_text_(uint8_t code) {
  switch (code) {
    case 0x00: return "No error";
    case 0x01: return "Overheat";
    case 0x02: return "Potential overheat";
    case 0x05: return "Flame sensor fault";
    case 0x06: return "Temperature sensor fault";
    case 0x09: return "Glow plug fault";
    case 0x0A: return "Motor RPM fault";
    case 0x0B: return "Air temperature fault";
    case 0x0C: return "Over voltage";
    case 0x0D: return "No start";
    case 0x0E: return "Water pump fault";
    case 0x0F: return "Under voltage";
    case 0x10: return "Ventilation duration";
    case 0x11: return "Fuel pump fault";
    case 0x14: return "No communication";
    case 0x1D: return "Flame blowout";
    case 0x1E: return "Flame detection";
    case 0x1F: return "Overheat (exit)";
    case 0x21: return "Control lockout";
    case 0x25: return "Locked (hard) - use unlock cmd";
    case 0x4E: return "Flame failure";
    default: {
      static char unknown_err[24];
      snprintf(unknown_err, sizeof(unknown_err), "Error %u (0x%02X)", code, code);
      return unknown_err;
    }
  }
}

// ===================
// Thermostat control
// ===================

void AutotermUART::configure_thermostat_mode(float target_c, uint8_t level, uint8_t sensor_source,
                                             float hys_on_c, float hys_off_c) {
  float clamped_target = clamp_thermostat_target_(target_c);
  uint8_t clamped_level = std::min<uint8_t>(level, 9);
  uint8_t clamped_sensor = clamp_temp_source_(sensor_source);
  float clamped_hys_on = clamp_thermostat_hys_on_(hys_on_c);
  float clamped_hys_off = clamp_thermostat_hys_off_(hys_off_c);

  bool was_active = thermostat_active_;
  bool log_needed = !was_active ||
                    thermostat_target_c_ != clamped_target ||
                    thermostat_level_ != clamped_level ||
                    thermostat_sensor_source_ != clamped_sensor ||
                    thermostat_hys_on_c_ != clamped_hys_on ||
                    thermostat_hys_off_c_ != clamped_hys_off;

  thermostat_active_ = true;
  thermostat_target_c_ = clamped_target;
  thermostat_level_ = clamped_level;
  thermostat_sensor_source_ = clamped_sensor;
  thermostat_hys_on_c_ = clamped_hys_on;

  // Restore eco-adaptive mode if it was configured in YAML
  if (eco_adaptive_configured_ && !eco_adaptive_active_) {
    eco_adaptive_active_ = true;
    eco_integral_ = 0.0f;
    eco_current_level_ = 0;
    eco_last_eval_ms_ = 0;
    ESP_LOGI("autoterm_uart", "eco-adaptive: restored from YAML config");
  }
  thermostat_hys_off_c_ = clamped_hys_off;

  // Reset PID state on reconfiguration
  pid_integral_ = 0.0f;
  pid_last_error_ = 0.0f;
  pid_last_eval_ms_ = 0;

  if (thermostat_last_sent_level_ == 255)
    thermostat_last_sent_level_ = thermostat_level_;

  if (thermostat_heating_request_ && thermostat_last_sent_level_ != thermostat_level_) {
    send_power_mode(false, thermostat_level_);
    thermostat_last_command_millis_ = millis();
    thermostat_last_sent_level_ = thermostat_level_;
  } else if (!thermostat_heating_request_) {
    thermostat_last_sent_level_ = thermostat_level_;
  }

  if (log_needed) {
    ESP_LOGI("autoterm_uart",
             "thermostat config -> target=%.1f°C level=%u sensor=%u hys_on=%.1f°C hys_off=%.1f°C",
             thermostat_target_c_, static_cast<unsigned>(thermostat_level_),
             static_cast<unsigned>(thermostat_sensor_source_),
             thermostat_hys_on_c_, thermostat_hys_off_c_);
  }

  evaluate_thermostat_control_(true);
}

void AutotermUART::disable_thermostat_mode() {
  if (!thermostat_active_ && !eco_adaptive_active_)
    return;

  if (thermostat_active_) {
    ESP_LOGI("autoterm_uart", "thermostat mode deactivated");
    thermostat_active_ = false;
    thermostat_heating_request_ = false;
    thermostat_waiting_for_idle_ = false;
    thermostat_last_sent_level_ = 255;
  }

  // Only deactivate eco-adaptive if it was NOT configured in YAML
  // (if configured, it will be restored when climate is re-enabled)
  if (eco_adaptive_active_ && !eco_adaptive_configured_) {
    ESP_LOGI("autoterm_uart", "eco-adaptive mode deactivated");
    eco_adaptive_active_ = false;
    eco_integral_ = 0.0f;
    eco_current_level_ = 0;
    eco_last_eval_ms_ = 0;
  } else if (eco_adaptive_active_ && eco_adaptive_configured_) {
    // Just reset the control state, keep active
    eco_integral_ = 0.0f;
    eco_current_level_ = 0;
    eco_last_eval_ms_ = 0;
    ESP_LOGD("autoterm_uart", "eco-adaptive: control state reset, will re-activate on next evaluation");
  }
}

void AutotermUART::evaluate_thermostat_control_(bool force) {
  if (!thermostat_active_ && !eco_adaptive_active_)
    return;

  uint32_t now = millis();
  if (!force && (now - thermostat_last_evaluation_millis_) < 1000)
    return;
  thermostat_last_evaluation_millis_ = now;

  // === ECO-ADAPTIVE MODE: continuous modulation with feed-forward ===
  if (eco_adaptive_active_) {
    evaluate_eco_adaptive_(force);
    return;
  }

  // === PID MODE: continuous power modulation ===
  if (pid_mode_active_) {
    uint8_t effective_source = get_effective_temp_source();
    if (effective_source != thermostat_sensor_source_)
      thermostat_sensor_source_ = clamp_temp_source_(effective_source);
    uint8_t source = thermostat_sensor_source_;
    float current_temp = get_temperature_for_source(source);
    if (!std::isfinite(current_temp))
      return;

    uint8_t new_level = compute_pid_output_(current_temp, thermostat_target_c_, now);

    // Start heater if not running
    if (!heater_running_ && thermostat_last_sent_level_ == 255) {
      send_power_mode(true, new_level);
      thermostat_last_sent_level_ = new_level;
      thermostat_heating_request_ = true;
      thermostat_last_command_millis_ = millis();
      ESP_LOGI("autoterm_uart", "PID: start heating at level %u (temp=%.1f°C target=%.1f°C)",
               new_level, current_temp, thermostat_target_c_);
    } else if (heater_running_ && new_level != thermostat_last_sent_level_ &&
               (now - thermostat_last_command_millis_) > 1500) {
      // Adjust power level smoothly
      send_power_mode(false, new_level);
      thermostat_last_sent_level_ = new_level;
      thermostat_last_command_millis_ = millis();
      ESP_LOGD("autoterm_uart", "PID: adjust level %u -> %u (temp=%.1f°C target=%.1f°C)",
               thermostat_last_sent_level_, new_level, current_temp, thermostat_target_c_);
    }

    // PID stop: if temp exceeds target by more than 2°C, go to standby
    if (current_temp > thermostat_target_c_ + 2.0f && thermostat_heating_request_) {
      send_standby();
      thermostat_heating_request_ = false;
      thermostat_last_sent_level_ = 255;
      pid_integral_ = 0.0f;  // Reset integral on stop
      ESP_LOGI("autoterm_uart", "PID: temp %.1f°C > target+2°C, standby", current_temp);
    }
    return;  // Skip hysteresis logic
  }

  // === HISTERESIS MODE: original on/off control ===
  uint8_t effective_source = get_effective_temp_source();
  if (effective_source != thermostat_sensor_source_)
    thermostat_sensor_source_ = clamp_temp_source_(effective_source);
  uint8_t source = thermostat_sensor_source_;

  float current_temp = get_temperature_for_source(source);
  if (!std::isfinite(current_temp))
    return;

  float on_threshold = thermostat_target_c_ - thermostat_hys_on_c_;
  float off_threshold = thermostat_target_c_ + thermostat_hys_off_c_;

  if (!thermostat_heating_request_ && !thermostat_waiting_for_idle_) {
    if (current_temp < on_threshold) {
      bool command_recent = thermostat_last_command_millis_ != 0 &&
                            (now - thermostat_last_command_millis_) < 1000;
      if (command_recent)
        return;

      // Proportional power: higher level when far from target, lower when close
      float error_magnitude = on_threshold - current_temp;  // How far below on_threshold
      uint8_t adaptive_level;
      if (error_magnitude > 4.0f) {
        adaptive_level = thermostat_level_;  // Full configured level
      } else if (error_magnitude > 2.0f) {
        adaptive_level = std::max(static_cast<uint8_t>(2),
                                  static_cast<uint8_t>(thermostat_level_ / 2));
      } else {
        adaptive_level = std::max(static_cast<uint8_t>(1),
                                  static_cast<uint8_t>(thermostat_level_ / 3));
      }

      bool heater_running_now = heater_running_;
      if (!heater_running_now) {
        send_power_mode(true, adaptive_level);
        thermostat_last_command_millis_ = millis();
      } else if (thermostat_last_sent_level_ != adaptive_level) {
        send_power_mode(false, adaptive_level);
        thermostat_last_command_millis_ = millis();
      }
      thermostat_last_sent_level_ = adaptive_level;
      thermostat_heating_request_ = true;
      ESP_LOGI("autoterm_uart",
               "thermostat: start heating (temp=%.1f°C target=%.1f°C level=%u, err=%.1f°C)",
               current_temp, thermostat_target_c_, static_cast<unsigned>(adaptive_level), error_magnitude);
    }
  } else if (thermostat_heating_request_) {
    if (current_temp > off_threshold) {
      bool command_recent = thermostat_last_command_millis_ != 0 &&
                            (now - thermostat_last_command_millis_) < 1000;
      if (command_recent)
        return;

      // Gentler cooldown: target - 2°C instead of -5°C, use heating mode instead of ventilation
      float cooldown_target = std::max(0.0f, thermostat_target_c_ - 2.0f);
      uint8_t temp_byte = static_cast<uint8_t>(std::round(std::min(30.0f, cooldown_target)));
      send_thermostat_cooldown_(source, temp_byte);
      thermostat_heating_request_ = false;
      thermostat_waiting_for_idle_ = true;
      thermostat_last_command_millis_ = millis();
      ESP_LOGI("autoterm_uart",
               "thermostat: cooling down (temp=%.1f°C target=%.1f°C -> temp_cmd=%u)",
               current_temp, thermostat_target_c_, static_cast<unsigned>(temp_byte));
    } else if (thermostat_last_sent_level_ != thermostat_level_ &&
               (now - thermostat_last_command_millis_) > 1500) {
      send_power_mode(false, thermostat_level_);
      thermostat_last_command_millis_ = now;
      thermostat_last_sent_level_ = thermostat_level_;
      ESP_LOGD("autoterm_uart", "thermostat: adjust level to %u",
               static_cast<unsigned>(thermostat_level_));
    }
  }
}

void AutotermUART::handle_thermostat_status_update_(uint16_t status_code) {
  if (!thermostat_active_)
    return;

  if (!thermostat_waiting_for_idle_ && !is_heater_active_status_(status_code))
    thermostat_heating_request_ = false;

  if (thermostat_waiting_for_idle_) {
    if (status_code == STATUS_IDLE_VENT || status_code == STATUS_FANS_ONLY) {
      ESP_LOGD("autoterm_uart", "thermostat: idle ventilation detected, sending standby");
      send_standby();
      thermostat_waiting_for_idle_ = false;
      thermostat_last_command_millis_ = millis();
    } else if (!is_heater_active_status_(status_code)) {
      thermostat_waiting_for_idle_ = false;
    }
  }
}

void AutotermUART::send_thermostat_cooldown_(uint8_t source, uint8_t temp_byte) {
  uint8_t sensor = map_source_to_heater_(source);
  uint8_t clamped_temp = std::min<uint8_t>(temp_byte, 30);
  std::vector<uint8_t> payload{0xFF, 0xFF, sensor, clamped_temp, 0x01, 0xFF};
  send_command_(FUNC_SETTINGS, payload, "mode.thermostat.cooldown");
}

// ===================
// Eco-Adaptive Mode: continuous power modulation
// ===================

void AutotermUART::evaluate_eco_adaptive_(bool force) {
  if (!eco_adaptive_active_)
    return;

  uint32_t now = millis();
  if (!force && (now - eco_last_eval_ms_) < 3000)  // Evaluate every 3 seconds (not 1s)
    return;
  eco_last_eval_ms_ = now;

  uint8_t effective_source = get_effective_temp_source();
  if (effective_source != thermostat_sensor_source_)
    thermostat_sensor_source_ = clamp_temp_source_(effective_source);
  uint8_t source = thermostat_sensor_source_;

  float current_temp = get_temperature_for_source(source);
  if (!std::isfinite(current_temp))
    return;

  float target = thermostat_target_c_;
  float error = target - current_temp;

  // Publish eco-adaptive sensors
  if (eco_adaptive_error_sensor_) eco_adaptive_error_sensor_->publish_state(error);

  // Deadband: if error is very small, keep current level
  if (std::fabs(error) < eco_deadband_ && eco_current_level_ > 0) {
    if (eco_adaptive_level_sensor_) eco_adaptive_level_sensor_->publish_state(eco_current_level_);
    if (eco_mode_status_sensor_) eco_mode_status_sensor_->publish_state("stable");
    return;
  }

  // Critical overshoot: stop immediately
  if (error < -2.0f && eco_current_level_ > 0) {
    send_standby();
    eco_current_level_ = 0;
    eco_integral_ = 0.0f;
    thermostat_heating_request_ = false;
    thermostat_last_sent_level_ = 255;
    ESP_LOGI("autoterm_uart", "ECO: critical overshoot %.1f°C > target+2, standby", current_temp);
    if (eco_adaptive_level_sensor_) eco_adaptive_level_sensor_->publish_state(0);
    if (eco_mode_status_sensor_) eco_mode_status_sensor_->publish_state("overshoot_stop");
    return;
  }

  // Compute PID terms
  float dt = 3.0f;  // Fixed interval (we evaluate every 3s)

  // Proportional
  float p_term = eco_kp_ * error;

  // Integral with anti-windup
  eco_integral_ += error * dt;
  if (eco_integral_ > PID_INTEGRAL_MAX) eco_integral_ = PID_INTEGRAL_MAX;
  if (eco_integral_ < -PID_INTEGRAL_MAX) eco_integral_ = -PID_INTEGRAL_MAX;
  float i_term = eco_ki_ * eco_integral_;

  // Derivative on measurement (not error)
  float derivative = 0.0f;
  if (std::isfinite(eco_last_temp_) && dt > 0.0f) {
    derivative = -(current_temp - eco_last_temp_) / dt;
  }
  eco_last_temp_ = current_temp;
  float d_term = eco_kd_ * derivative;

  // Overshoot prediction: if temperature is rising fast toward target, reduce output
  float output = p_term + i_term + d_term;
  if (eco_overshoot_predict_ && derivative > 0.3f && error < 2.0f) {
    // Temperature rising fast and getting close — dampen the output
    float dampening = std::min(1.0f, derivative * 0.5f);
    output *= (1.0f - dampening);
    ESP_LOGD("autoterm_uart", "ECO: overshoot predict dampening %.1f (deriv=%.2f)", dampening, derivative);
  }

  // Exhaust temperature feedback: if combustion is efficient, reduce output
  // High efficiency = exhaust temp is high relative to pump = good combustion = can reduce fuel
  if (combustion_efficiency_pct_ > 0.0f && std::isfinite(combustion_efficiency_pct_)) {
    if (combustion_efficiency_pct_ > 80.0f) {
      // Very efficient combustion — reduce output by 10%
      output *= 0.9f;
      ESP_LOGD("autoterm_uart", "ECO: efficiency feedback %.0f%% -> output x0.9", combustion_efficiency_pct_);
    } else if (combustion_efficiency_pct_ < 40.0f) {
      // Poor combustion — increase output by 10% to compensate
      output *= 1.1f;
      ESP_LOGD("autoterm_uart", "ECO: efficiency feedback %.0f%% -> output x1.1", combustion_efficiency_pct_);
    }
  }

  // Anti-condensation boost: if exhaust temp drops below condensation point, increase power
  // This prevents water accumulation in the exhaust system which causes rust and damage
  if (std::isfinite(last_exhaust_temp_c_) && last_exhaust_temp_c_ < ANTI_CONDENSATION_TEMP_C &&
      last_exhaust_temp_c_ > 50.0f && heater_running_) {
    // Boost output by 20% to raise exhaust temp above condensation point
    output *= 1.2f;
    ESP_LOGD("autoterm_uart", "ECO: anti-condensation boost (exh=%.0f°C < %.0f°C) -> output x1.2",
             last_exhaust_temp_c_, ANTI_CONDENSATION_TEMP_C);
  }

  // Map output to level 1-9
  float max_output = eco_kp_ * 5.0f;  // Reference: error=5°C at max gain
  float normalized = output / max_output;
  int level = static_cast<int>(std::round(1.0f + normalized * 8.0f));
  level = std::max(static_cast<int>(eco_min_level_), std::min(static_cast<int>(eco_max_level_), level));

  // Apply night mode cap
  if (night_mode_active_ && level > static_cast<int>(night_mode_max_level_)) {
    level = static_cast<int>(night_mode_max_level_);
  }

  // Apply altitude compensation cap (from GPS HA sensor)
  if (air_density_factor_ < 0.95f) {
    int altitude_max = static_cast<int>(9 * air_density_factor_);
    altitude_max = std::max(1, altitude_max);  // Never go below level 1
    if (level > altitude_max) {
      ESP_LOGD("autoterm_uart", "ECO: altitude cap %u -> %u (density=%.2f)",
               level, altitude_max, air_density_factor_);
      level = altitude_max;
    }
  }

  // Burnout protection: force minimum level during first 4 minutes after ignition
  // Ensures complete combustion and prevents soot buildup during critical warm-up
  if (burnout_protection_active_ && level < static_cast<int>(BURNOUT_MIN_LEVEL)) {
    ESP_LOGD("autoterm_uart", "ECO: burnout floor %u -> %u (min level during warm-up)",
             level, static_cast<unsigned>(BURNOUT_MIN_LEVEL));
    level = static_cast<int>(BURNOUT_MIN_LEVEL);
  }

  uint8_t new_level = static_cast<uint8_t>(level);

  // Start heater if not running
  if (!heater_running_ || eco_current_level_ == 0) {
    send_power_mode(true, new_level);
    eco_current_level_ = new_level;
    eco_last_command_ms_ = now;
    thermostat_heating_request_ = true;
    thermostat_last_sent_level_ = new_level;
    ESP_LOGI("autoterm_uart", "ECO: start at level %u (temp=%.1f°C target=%.1f°C err=%.1f°C)",
             new_level, current_temp, target, error);
  } else if (new_level != eco_current_level_ && (now - eco_last_command_ms_) > 3000) {
    // Only change level every 3 seconds to avoid rapid switching
    send_power_mode(false, new_level);
    uint8_t old_level = eco_current_level_;
    eco_current_level_ = new_level;
    eco_last_command_ms_ = now;
    thermostat_last_sent_level_ = new_level;
    ESP_LOGD("autoterm_uart", "ECO: level %u -> %u (temp=%.1f°C target=%.1f°C err=%.1f°C)",
             old_level, new_level, current_temp, target, error);
  }

  // Publish sensors
  if (eco_adaptive_level_sensor_) eco_adaptive_level_sensor_->publish_state(eco_current_level_);
  if (eco_mode_status_sensor_) {
    if (error > 2.0f) eco_mode_status_sensor_->publish_state("heating");
    else if (error > eco_deadband_) eco_mode_status_sensor_->publish_state("warming");
    else if (error > -eco_deadband_) eco_mode_status_sensor_->publish_state("stable");
    else eco_mode_status_sensor_->publish_state("cooling");
  }

  // Power efficiency tracking
  if (eco_power_efficiency_sensor_ && eco_current_level_ > 0) {
    // Efficiency = inverse of power level (lower level = more efficient)
    float efficiency = (1.0f - static_cast<float>(eco_current_level_ - eco_min_level_) /
                        static_cast<float>(eco_max_level_ - eco_min_level_)) * 100.0f;
    eco_power_efficiency_sensor_->publish_state(efficiency);
  }
}

float AutotermUART::clamp_thermostat_target_(float target) const {
  if (target < 0.0f)
    return 0.0f;
  if (target > 30.0f)
    return 30.0f;
  return target;
}

float AutotermUART::clamp_thermostat_hys_on_(float value) const {
  if (value < 1.0f)
    return 1.0f;
  if (value > 5.0f)
    return 5.0f;
  return value;
}

float AutotermUART::clamp_thermostat_hys_off_(float value) const {
  if (value < 0.0f)
    return 0.0f;
  if (value > 2.0f)
    return 2.0f;
  return value;
}

// ===================
// PID Controller
// ===================
uint8_t AutotermUART::compute_pid_output_(float current_temp, float target_temp, uint32_t now) {
  float error = target_temp - current_temp;

  // First evaluation: compute level based on actual error (not hardcoded 4)
  if (pid_last_eval_ms_ == 0) {
    pid_last_eval_ms_ = now;
    pid_last_error_ = error;
    eco_last_temp_ = current_temp;
    pid_integral_ = 0.0f;
    pid_phase_start_ms_ = now;
    pid_phase_ = PID_PHASE_STARTUP;
    // Map initial error to level: error=0 -> level=1, error=5+ -> level=9
    float initial_output = PID_GAINS[PID_PHASE_STARTUP][0] * error;  // Use startup Kp
    int level = static_cast<int>(std::round(1.0f + (initial_output / (pid_kp_ * 5.0f)) * 8.0f));
    level = std::max(static_cast<int>(eco_min_level_), std::min(static_cast<int>(eco_max_level_), level));
    pid_output_history_[0] = static_cast<float>(level);
    pid_output_history_[1] = static_cast<float>(level);
    pid_output_history_[2] = static_cast<float>(level);
    pid_output_index_ = 0;
    pid_output_initialized_ = true;
    ESP_LOGI("autoterm_uart", "PID: init err=%.1f°C -> level=%d", error, level);
    return static_cast<uint8_t>(level);
  }

  float dt = static_cast<float>(now - pid_last_eval_ms_) / 1000.0f;
  if (dt < 0.5f) dt = 0.5f;  // Minimum 0.5s between evaluations
  pid_last_eval_ms_ = now;

  // Deadband: if error is very small, keep current level (FIX: check error, not integral)
  if (std::fabs(error) < PID_DEADBAND_C) {
    return thermostat_level_;
  }

  // === PID GAIN SCHEDULING ===
  // Detect phase based on error magnitude and time since start
  PIDPhase new_phase;
  uint32_t time_since_start = now - pid_phase_start_ms_;

  if (time_since_start < 300000) {  // First 5 minutes
    new_phase = PID_PHASE_STARTUP;
  } else if (std::fabs(error) < 2.0f) {
    new_phase = PID_PHASE_STEADY;
  } else {
    new_phase = PID_PHASE_APPROACHING;
  }

  // Apply phase gains
  if (new_phase != pid_phase_) {
    pid_phase_ = new_phase;
    pid_integral_ *= 0.5f;  // Reduce integral on phase change to prevent windup
    ESP_LOGI("autoterm_uart", "PID phase -> %s (Kp=%.1f Ki=%.1f Kd=%.1f)",
             new_phase == PID_PHASE_STARTUP ? "STARTUP" :
             new_phase == PID_PHASE_APPROACHING ? "APPROACHING" : "STEADY",
             PID_GAINS[new_phase][0], PID_GAINS[new_phase][1], PID_GAINS[new_phase][2]);
  }

  // Use phase-based gains instead of fixed gains
  float kp = PID_GAINS[pid_phase_][0];
  float ki = PID_GAINS[pid_phase_][1];
  float kd = PID_GAINS[pid_phase_][2];

  // Proportional term
  float p_term = kp * error;

  // Integral term with anti-windup (uses phase-based Ki)
  pid_integral_ += error * dt;
  if (pid_integral_ > PID_INTEGRAL_MAX) pid_integral_ = PID_INTEGRAL_MAX;
  if (pid_integral_ < -PID_INTEGRAL_MAX) pid_integral_ = -PID_INTEGRAL_MAX;
  float i_term = ki * pid_integral_;

  // Derivative term on MEASUREMENT (not error) — avoids derivative kick on setpoint changes
  // Uses phase-based Kd (0 during startup to avoid noise)
  float derivative = 0.0f;
  if (std::isfinite(eco_last_temp_) && dt > 0.0f) {
    derivative = -(current_temp - eco_last_temp_) / dt;  // Negative because d(error)/dt = -d(temp)/dt
  }
  eco_last_temp_ = current_temp;
  float d_term = kd * derivative;
  pid_last_error_ = error;

  // Raw output
  float raw_output = p_term + i_term + d_term;

  // Map to level 1-9: output=0 -> level=1, output=max -> level=9
  float max_output = kp * 5.0f;  // Output when error = 5°C (uses phase-based Kp)
  float normalized = raw_output / max_output;  // Roughly [0, 1] for typical errors
  int level = static_cast<int>(std::round(1.0f + normalized * 8.0f));
  level = std::max(static_cast<int>(eco_min_level_), std::min(static_cast<int>(eco_max_level_), level));

  // Output smoothing: rolling average of last 3 levels to prevent oscillation
  pid_output_history_[pid_output_index_] = static_cast<float>(level);
  pid_output_index_ = (pid_output_index_ + 1) % 3;
  if (pid_output_initialized_) {
    float smoothed = (pid_output_history_[0] + pid_output_history_[1] + pid_output_history_[2]) / 3.0f;
    level = static_cast<int>(std::round(smoothed));
    level = std::max(static_cast<int>(eco_min_level_), std::min(static_cast<int>(eco_max_level_), level));
  }

  ESP_LOGD("autoterm_uart", "PID: err=%.1f°C P=%.1f I=%.1f D=%.1f raw=%.1f -> level=%d",
           error, p_term, i_term, d_term, raw_output, level);

  return static_cast<uint8_t>(level);
}

void AutotermUART::request_settings() {
  if (send_command_(FUNC_SETTINGS, {}, "request.settings"))
    last_settings_request_millis_ = millis();
}

void AutotermUART::send_status_request() {
  if (send_command_(FUNC_STATUS, {}, "request.status"))
    last_status_request_millis_ = millis();
}

void AutotermUART::send_panel_temperature_override_frame_() {
  if (!uart_heater_)
    return;
  if (!std::isfinite(panel_temp_override_value_c_))
    return;

  uint8_t temp_byte = compute_override_temperature_byte_();

  std::vector<uint8_t> frame{FRAME_HEADER, DEVICE_DISPLAY, 0x01, 0x00, FUNC_PANEL_TEMP, temp_byte};
  append_crc_(frame);

  uart_heater_->write_array(frame);
  uart_heater_->flush();

  panel_temp_last_value_c_ = panel_temp_override_value_c_;
  if (panel_temp_sensor_ != nullptr)
    panel_temp_sensor_->publish_state(panel_temp_override_value_c_);

  ESP_LOGD("autoterm_uart", "Panel temperature override frame sent: byte=%u (%.1f°C)",
           static_cast<unsigned>(temp_byte), panel_temp_override_value_c_);
}

// ===================
// AutotermClimate Implementations
// ===================

void AutotermClimate::set_parent(AutotermUART *parent) {
  parent_ = parent;
  preset_mode_ = sanitize_preset_(preset_mode_);
  this->mode = climate::CLIMATE_MODE_OFF;
  this->action = climate::CLIMATE_ACTION_OFF;
  this->fan_mode.reset();
  fan_level_ = clamp_level_(fan_level_);
  {
    std::string fan_label = fan_mode_label_from_level_(fan_level_);
    if (!fan_label.empty())
      this->set_custom_fan_mode_(fan_label.c_str()); // FIX: Using .c_str()
    else
      this->set_fan_mode_(climate::CLIMATE_FAN_OFF);
  }
  this->preset.reset();
  if (!preset_mode_.empty())
    this->set_custom_preset_(preset_mode_.c_str()); // FIX: Using .c_str()
  else
    this->set_preset_(climate::CLIMATE_PRESET_NONE);

  target_temperature_c_ = clamp_temperature_(target_temperature_c_);
  this->target_temperature = target_temperature_c_;
  if (!std::isnan(current_temperature_c_))
    this->current_temperature = current_temperature_c_;
  else
    this->current_temperature = NAN;
}

void AutotermClimate::set_default_level(uint8_t level) {
  fan_level_ = clamp_level_(level);
  this->fan_mode.reset();
  std::string fan_label = fan_mode_label_from_level_(fan_level_);
  if (!fan_label.empty())
    this->set_custom_fan_mode_(fan_label.c_str()); // FIX: Using .c_str()
  else
    this->set_fan_mode_(climate::CLIMATE_FAN_OFF);
}

void AutotermClimate::set_default_temperature(float temperature_c) {
  target_temperature_c_ = clamp_temperature_(temperature_c);
  this->target_temperature = target_temperature_c_;
}

void AutotermClimate::set_default_temp_sensor(uint8_t sensor) {
  if (sensor < 1)
    sensor = 1;
  if (sensor > 4)
    sensor = 4;
  default_temp_sensor_ = sensor;
}

void AutotermClimate::set_thermostat_hysteresis(float hys_on_c, float hys_off_c) {
  thermostat_hys_on_c_ = clamp_hysteresis_on_(hys_on_c);
  thermostat_hys_off_c_ = clamp_hysteresis_off_(hys_off_c);
  if (thermostat_hys_off_c_ > thermostat_hys_on_c_)
    thermostat_hys_off_c_ = thermostat_hys_on_c_;
}

climate::ClimateTraits AutotermClimate::traits() {
  climate::ClimateTraits traits;

  traits.set_supports_two_point_target_temperature(false);
  traits.set_supports_action(true);

  // Temperature limits
  traits.set_visual_min_temperature(0.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(1.0f);

  // FIXED: Store strings persistently to avoid dangling pointers
  preset_strings_ = {"power_mode", "heating", "heating+ventilation", "thermostat"};
  fan_mode_strings_.clear();
  for (int i = 0; i <= 9; i++) {
    fan_mode_strings_.push_back("Level " + std::to_string(i));
  }

  // Convert to const char* vectors
  std::vector<const char*> presets_ptr;
  for (const auto &s : preset_strings_) {
    presets_ptr.push_back(s.c_str());
  }

  std::vector<const char*> fan_modes_ptr;
  for (const auto &s : fan_mode_strings_) {
    fan_modes_ptr.push_back(s.c_str());
  }

  traits.set_supported_custom_presets(presets_ptr);
  traits.set_supported_custom_fan_modes(fan_modes_ptr);

  // Supported modes
  std::set<climate::ClimateMode> modes;
  modes.insert(climate::CLIMATE_MODE_OFF);
  modes.insert(climate::CLIMATE_MODE_HEAT);
  modes.insert(climate::CLIMATE_MODE_FAN_ONLY);
  traits.set_supported_modes(modes);

  // Note: set_supports_current_temperature is deprecated in 2025.11
  // Current temperature is auto-detected when current_temperature is set

  return traits;
}

void AutotermClimate::control(const climate::ClimateCall &call) {
  climate::ClimateMode new_mode = this->mode;
  if (call.get_mode().has_value())
    new_mode = *call.get_mode();

  std::string new_preset = preset_mode_;
  bool preset_overridden = false;
  if (call.get_custom_preset() != nullptr) {
    new_preset = sanitize_preset_(call.get_custom_preset());
    preset_overridden = true;
  } else if (call.get_preset().has_value()) {
    new_preset = sanitize_preset_(preset_from_enum_(*call.get_preset()));
    preset_overridden = true;
  }

  if (!preset_overridden) {
    switch (new_mode) {
      case climate::CLIMATE_MODE_FAN_ONLY:
      case climate::CLIMATE_MODE_OFF:
        new_preset.clear();
        break;
      case climate::CLIMATE_MODE_AUTO:
        if (new_preset.empty())
          new_preset = "heating+ventilation";
        break;
      case climate::CLIMATE_MODE_HEAT:
      default:
        if (new_preset.empty())
          new_preset = "power_mode";
        break;
    }
    if (!new_preset.empty())
      new_preset = sanitize_preset_(new_preset);
  }

  uint8_t new_level = fan_level_;
  if (call.get_custom_fan_mode() != nullptr)
    new_level = fan_mode_label_to_level_(call.get_custom_fan_mode());
  else if (call.get_fan_mode().has_value())
    new_level = fan_level_from_enum_(*call.get_fan_mode(), fan_level_);
  new_level = clamp_level_(new_level);

  float new_target_temp = target_temperature_c_;
  if (call.get_target_temperature().has_value())
    new_target_temp = clamp_temperature_(*call.get_target_temperature());

  ESP_LOGD("autoterm_uart", "Climate control -> mode=%d preset=%s level=%u target=%.1f°C",
           static_cast<int>(new_mode), new_preset.c_str(), new_level, new_target_temp);

  if (!parent_) {
    ESP_LOGW("autoterm_uart", "Climate control requested without parent link");
    apply_state_(new_mode, new_preset, new_level, new_target_temp);
    return;
  }

  climate::ClimateMode previous_mode = this->mode;
  bool should_start = previous_mode == climate::CLIMATE_MODE_OFF ||
                      previous_mode == climate::CLIMATE_MODE_FAN_ONLY ||
                      !parent_->settings_valid_;

  if (new_mode == climate::CLIMATE_MODE_OFF) {
    parent_->disable_thermostat_mode();
    parent_->send_standby();
  } else if (new_mode == climate::CLIMATE_MODE_FAN_ONLY) {
    parent_->disable_thermostat_mode();
    if (previous_mode != climate::CLIMATE_MODE_OFF)
      parent_->send_standby();
    parent_->send_fan_only(new_level);
  } else {
    if (previous_mode == climate::CLIMATE_MODE_FAN_ONLY)
      parent_->send_standby();

    if (new_preset == "power_mode") {
      parent_->disable_thermostat_mode();
      parent_->send_power_mode(should_start, new_level);
    } else if (new_preset == "heating") {
      parent_->disable_thermostat_mode();
      uint8_t sensor = resolve_temp_sensor_();
      uint8_t temp_byte = static_cast<uint8_t>(std::round(new_target_temp));
      parent_->send_temperature_hold_mode(should_start, sensor, temp_byte);
    } else if (new_preset == "heating+ventilation") {
      parent_->disable_thermostat_mode();
      uint8_t sensor = resolve_temp_sensor_();
      uint8_t temp_byte = static_cast<uint8_t>(std::round(new_target_temp));
      parent_->send_temperature_to_fan_mode(should_start, sensor, temp_byte);
    } else if (new_preset == "thermostat") {
      uint8_t sensor = resolve_temp_sensor_();
      parent_->configure_thermostat_mode(new_target_temp, new_level, sensor,
                                         thermostat_hys_on_c_, thermostat_hys_off_c_);
    } else {
      parent_->disable_thermostat_mode();
    }
  }

  apply_state_(new_mode, new_preset, new_level, new_target_temp);
}

void AutotermClimate::handle_status_update(uint16_t status_code, float internal_temp) {
  bool changed = false;
  float display_temp = internal_temp;
  if (parent_ != nullptr) {
    uint8_t source = parent_->get_effective_temp_source();
    float resolved = parent_->get_temperature_for_source(source);
    if (std::isfinite(resolved))
      display_temp = resolved;
  }

  if (!std::isnan(display_temp)) {
    if (std::isnan(current_temperature_c_) || std::fabs(display_temp - current_temperature_c_) > 0.1f) {
      current_temperature_c_ = display_temp;
      this->current_temperature = display_temp;
      changed = true;
    }
  }

  climate::ClimateAction previous_action = this->action;
  update_action_from_status_(status_code);
  if (this->action != previous_action)
    changed = true;

  if (changed)
    this->publish_state();
}

void AutotermClimate::handle_settings_update(const AutotermUART::Settings &settings, bool from_display) {
  if (!from_display)
    return;
  uint8_t level = clamp_level_(settings.power_level);
  float target = clamp_temperature_(static_cast<float>(settings.set_temperature));
  std::string preset = deduce_preset_from_settings_(settings);
  climate::ClimateMode mode = deduce_mode_from_settings_(settings);
  apply_state_(mode, preset, level, target);
}

uint8_t AutotermClimate::clamp_level_(int level) {
  if (level < 0)
    return 0;
  if (level > 9)
    return 9;
  return static_cast<uint8_t>(level);
}

float AutotermClimate::clamp_temperature_(float temperature) {
  if (temperature < 0.0f)
    return 0.0f;
  if (temperature > 30.0f)
    return 30.0f;
  return temperature;
}

float AutotermClimate::clamp_hysteresis_on_(float value) {
  if (value < 1.0f)
    return 1.0f;
  if (value > 5.0f)
    return 5.0f;
  return value;
}

float AutotermClimate::clamp_hysteresis_off_(float value) {
  if (value < 0.0f)
    return 0.0f;
  if (value > 2.0f)
    return 2.0f;
  return value;
}

std::string AutotermClimate::fan_mode_label_from_level_(uint8_t level) const {
  level = clamp_level_(level);
  return "Level " + std::to_string(static_cast<int>(level));
}

uint8_t AutotermClimate::fan_mode_label_to_level_(const std::string &label) const {
  const std::string prefix = "Level ";
  if (label.size() <= prefix.size() || label.compare(0, prefix.size(), prefix) != 0)
    return fan_level_;
  std::string digits = label.substr(prefix.size());
  if (digits.empty())
    return fan_level_;
  int value = 0;
  for (char c : digits) {
    if (!std::isdigit(static_cast<unsigned char>(c)))
      return fan_level_;
    value = value * 10 + (c - '0');
  }
  return clamp_level_(value);
}

std::string AutotermClimate::sanitize_preset_(const std::string &preset) const {
  if (preset == "power_mode" || preset == "heating" || preset == "heating+ventilation" || preset == "thermostat")
    return preset;
  return preset_mode_;
}

uint8_t AutotermClimate::resolve_temp_sensor_() const {
  if (parent_ != nullptr) {
    uint8_t manual = parent_->get_manual_temp_source();
    if (manual >= 1 && manual <= 4)
      return manual;
    if (parent_->settings_valid_) {
      uint8_t src = parent_->settings_.temperature_source;
      if (src >= 1 && src <= 4)
        return src;
    }
  }
  uint8_t sensor = default_temp_sensor_;
  if (sensor < 1)
    sensor = 1;
  if (sensor > 4)
    sensor = 4;
  return sensor;
}

climate::ClimateMode AutotermClimate::deduce_mode_from_settings_(const AutotermUART::Settings &settings) const {
  if (settings.wait_mode == 0x00 && settings.power_level == 0)
    return climate::CLIMATE_MODE_OFF;
  if (settings.temperature_source == 4)  // Home Assistant source
    return climate::CLIMATE_MODE_HEAT;
  if (settings.wait_mode == 0x01)
    return climate::CLIMATE_MODE_AUTO;
  if (settings.wait_mode == 0x02)
    return climate::CLIMATE_MODE_HEAT;
  return this->mode;
}

std::string AutotermClimate::deduce_preset_from_settings_(const AutotermUART::Settings &settings) const {
  if (settings.temperature_source == 4)  // Home Assistant source
    return "power_mode";
  if (settings.wait_mode == 0x01)
    return "heating+ventilation";
  if (settings.wait_mode == 0x02)
    return "heating";
  return preset_mode_;
}

std::string AutotermClimate::preset_from_enum_(climate::ClimatePreset preset) {
  switch (preset) {
    case climate::CLIMATE_PRESET_NONE:
      return "power_mode";
    case climate::CLIMATE_PRESET_HOME:
    case climate::CLIMATE_PRESET_COMFORT:
    case climate::CLIMATE_PRESET_SLEEP:
      return "heating";
    case climate::CLIMATE_PRESET_AWAY:
    case climate::CLIMATE_PRESET_ACTIVITY:
      return "heating+ventilation";
    case climate::CLIMATE_PRESET_BOOST:
      return "power_mode";
    case climate::CLIMATE_PRESET_ECO:
      return "thermostat";
    default:
      return "";
  }
}

uint8_t AutotermClimate::fan_level_from_enum_(climate::ClimateFanMode mode, uint8_t fallback_level) {
  switch (mode) {
    case climate::CLIMATE_FAN_OFF:
      return 0;
    case climate::CLIMATE_FAN_LOW:
      return 1;
    case climate::CLIMATE_FAN_MEDIUM:
      return 4;
    case climate::CLIMATE_FAN_MIDDLE:
      return 5;
    case climate::CLIMATE_FAN_HIGH:
      return 7;
    case climate::CLIMATE_FAN_ON:
      return 9;
    case climate::CLIMATE_FAN_FOCUS:
      return 8;
    case climate::CLIMATE_FAN_DIFFUSE:
      return 3;
    case climate::CLIMATE_FAN_QUIET:
      return 2;
    case climate::CLIMATE_FAN_AUTO:
      return fallback_level;
    default:
      return fallback_level;
  }
}

void AutotermClimate::apply_state_(climate::ClimateMode mode, const std::string &preset, uint8_t level, float target_temp) {
  preset_mode_ = sanitize_preset_(preset);
  fan_level_ = clamp_level_(level);
  target_temperature_c_ = clamp_temperature_(target_temp);

  this->mode = mode;

  // FIX: Using setters for preset with .c_str()
  this->preset.reset();
  if (mode != climate::CLIMATE_MODE_FAN_ONLY && mode != climate::CLIMATE_MODE_OFF && !preset_mode_.empty())
    this->set_custom_preset_(preset_mode_.c_str());
  else
    this->set_preset_(climate::CLIMATE_PRESET_NONE);

  // FIX: Using setters for fan mode with .c_str()
  this->fan_mode.reset();
  {
    std::string fan_label = fan_mode_label_from_level_(fan_level_);
    if (!fan_label.empty())
      this->set_custom_fan_mode_(fan_label.c_str());
    else
      this->set_fan_mode_(climate::CLIMATE_FAN_OFF);
  }

  this->target_temperature = target_temperature_c_;
  if (!std::isnan(current_temperature_c_))
    this->current_temperature = current_temperature_c_;
  else
    this->current_temperature = NAN;

  if (mode == climate::CLIMATE_MODE_OFF)
    this->action = climate::CLIMATE_ACTION_OFF;
  else if (mode == climate::CLIMATE_MODE_FAN_ONLY)
    this->action = climate::CLIMATE_ACTION_FAN;
  else
    this->action = climate::CLIMATE_ACTION_HEATING;

  this->publish_state();
}

void AutotermClimate::update_action_from_status_(uint16_t status_code) {
  climate::ClimateAction action = climate::CLIMATE_ACTION_IDLE;
  switch (status_code) {
    case 0x0000:
    case STATUS_STANDBY:
      action = this->mode == climate::CLIMATE_MODE_OFF ? climate::CLIMATE_ACTION_OFF
                                                       : climate::CLIMATE_ACTION_IDLE;
      break;
    case STATUS_VENTILATION:
    case STATUS_FANS_ONLY:
      action = climate::CLIMATE_ACTION_FAN;
      break;
    case 0x0200:
    case 0x0201:
    case 0x0202:
    case 0x0203:
    case 0x0204:
    case STATUS_HEATING:
      action = climate::CLIMATE_ACTION_HEATING;
      break;
    case STATUS_COOLING:
    case STATUS_SHUTDOWN:
      action = climate::CLIMATE_ACTION_IDLE;
      break;
    default:
      if (this->mode == climate::CLIMATE_MODE_OFF)
        action = climate::CLIMATE_ACTION_OFF;
      else if (this->mode == climate::CLIMATE_MODE_FAN_ONLY)
        action = climate::CLIMATE_ACTION_FAN;
      else
        action = climate::CLIMATE_ACTION_IDLE;
      break;
  }
  this->action = action;
}

}  // namespace autoterm_uart
}  // namespace esphome
