/**
 * @file autoterm_uart.h
 * @brief ESPHome bidirectional UART bridge for Autoterm/Planar diesel heaters.
 *
 * Sits between the physical display panel and the heater unit, sniffing and
 * forwarding all UART frames while exposing sensors, climate control, and
 * thermostat logic to Home Assistant via ESPHome.
 */

#pragma once
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/button/button.h"
#include "esphome/core/time.h"
#include "esphome/core/preferences.h"
#include "esphome/core/helpers.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace autoterm_uart {

// Protocol constants
static constexpr uint8_t FRAME_HEADER = 0xAA;
static constexpr uint8_t DEVICE_DISPLAY = 0x03;
static constexpr uint8_t DEVICE_HEATER = 0x04;
static constexpr uint8_t FUNC_STATUS = 0x0F;
static constexpr uint8_t FUNC_SETTINGS = 0x02;
static constexpr uint8_t FUNC_POWER_START = 0x01;
static constexpr uint8_t FUNC_STANDBY = 0x03;
static constexpr uint8_t FUNC_PANEL_TEMP = 0x11;
static constexpr uint8_t FUNC_FAN_ONLY = 0x23;
// Extended protocol commands (from schroeder-robert / kalutep documentation)
static constexpr uint8_t FUNC_SERIAL_NUM = 0x04;
static constexpr uint8_t FUNC_VERSION = 0x06;
static constexpr uint8_t FUNC_DIAGNOSTIC = 0x07;
static constexpr uint8_t FUNC_SET_FAN = 0x08;
static constexpr uint8_t FUNC_REPORT = 0x0B;
static constexpr uint8_t FUNC_UNLOCK = 0x0D;
static constexpr uint8_t FUNC_PRIME = 0x13;
static constexpr uint8_t FUNC_HANDSHAKE = 0x1C;

// Device IDs
static constexpr uint8_t DEVICE_BOOT = 0x00;
static constexpr uint8_t DEVICE_DIAG = 0x02;

// Status codes
static constexpr uint16_t STATUS_SLEEP = 0x0000;
static constexpr uint16_t STATUS_STANDBY = 0x0001;
static constexpr uint16_t STATUS_FLAME_COOL = 0x0100;
static constexpr uint16_t STATUS_VENTILATION = 0x0101;
static constexpr uint16_t STATUS_PREHEAT = 0x0200;
static constexpr uint16_t STATUS_GLOW_PLUG = 0x0201;
static constexpr uint16_t STATUS_IGNITION_1 = 0x0202;
static constexpr uint16_t STATUS_IGNITION_2 = 0x0203;
static constexpr uint16_t STATUS_RAMP_UP = 0x0204;
static constexpr uint16_t STATUS_HEATING = 0x0300;
static constexpr uint16_t STATUS_COOLING = 0x0304;
static constexpr uint16_t STATUS_IDLE_VENT = 0x0305;
static constexpr uint16_t STATUS_FANS_ONLY = 0x0323;
static constexpr uint16_t STATUS_SHUTDOWN = 0x0400;

// Timing constants
static constexpr uint32_t DISPLAY_TIMEOUT_MS = 5000;
static constexpr uint32_t STATUS_REQUEST_INTERVAL_MS = 2000;
static constexpr uint32_t SETTINGS_REQUEST_INTERVAL_MS = 10000;
static constexpr uint32_t PANEL_TEMP_INTERVAL_MS = 1000;
static constexpr uint32_t FRAME_TIMEOUT_MS = 200;
static constexpr uint32_t UART_LOSS_TIMEOUT_MS = 10000;
static constexpr uint32_t COMMAND_RATE_LIMIT_MS = 1000;
static constexpr uint32_t CRC_SAVE_INTERVAL_MS = 60000;
static constexpr uint32_t UART_LOST_TIMEOUT_MS = 10000;

// Fuel consumption estimation (Autoterm Air 4D)
// Approx: 0.15L/h at pump 2Hz, 0.5L/h at pump 6Hz
static constexpr float FUEL_LITERS_PER_HZ = 0.085f;

// Maintenance intervals (hours)
static constexpr float MAINTENANCE_OIL_HOURS = 500.0f;
static constexpr float MAINTENANCE_FILTER_HOURS = 200.0f;
static constexpr float MAINTENANCE_GLOW_HOURS = 1000.0f;

// Frost protection threshold
static constexpr float FROST_PROTECTION_TEMP_C = 2.0f;

// Safety thresholds
static constexpr float SAFETY_MAX_HEATER_TEMP_C = 400.0f;     // Emergency shutdown above this
static constexpr float SAFETY_FLAMEOUT_MIN_TEMP_C = 100.0f;   // Below this = possible flameout
static constexpr uint32_t SAFETY_FLAMEOUT_TIMEOUT_MS = 90000;  // 90s with pump active + low temp (increased to reduce false triggers)
static constexpr float SAFETY_MIN_STARTUP_VOLTAGE_V = 10.0f;  // Below this, refuse to start

// PID controller defaults (for thermostat mode)
static constexpr float PID_KP_DEFAULT = 2.0f;   // Proportional gain
static constexpr float PID_KI_DEFAULT = 0.5f;   // Integral gain
static constexpr float PID_KD_DEFAULT = 0.1f;   // Derivative gain
static constexpr float PID_DEADBAND_C = 0.5f;   // No action if error < this
static constexpr float PID_INTEGRAL_MAX = 5.0f;  // Anti-windup clamp

// PID Gain Scheduling phases
enum PIDPhase : uint8_t {
  PID_PHASE_STARTUP = 0,     // First 5 minutes: aggressive heating
  PID_PHASE_APPROACHING = 1, // Error < 2°C: reduce overshoot
  PID_PHASE_STEADY = 2,      // Near target: precise maintenance
};

// Gain tables per phase (startup / approaching / steady)
static constexpr float PID_GAINS[][3] = {
  // {Kp,    Ki,    Kd}     Phase
  { 3.0f,  0.2f,  0.0f },  // STARTUP: fast rise, no derivative
  { 1.5f,  0.4f,  0.3f },  // APPROACHING: moderate, anticipate overshoot
  { 1.0f,  0.6f,  0.1f },  // STEADY: slow integral, precise maintenance
};

class AutotermUART;      // Forward declaration
class AutotermClimate;   // Forward declaration

// ===================
// Custom Number Class
// ===================
class AutotermFanLevelNumber : public number::Number {
 public:
  AutotermUART *parent_{nullptr};
  void setup_parent(AutotermUART *p) { parent_ = p; }

 protected:
  void control(float value) override;
};

class AutotermTempSourceSelect : public select::Select {
 public:
  void set_parent(AutotermUART *parent);
  void publish_for_source(uint8_t source);

 protected:
  void control(const std::string &value) override;

 private:
  AutotermUART *parent_{nullptr};
  const char *option_from_source_(uint8_t source) const;
  uint8_t source_from_option_(const std::string &option) const;
};

// ===================
// Custom Button Classes
// ===================
class AutotermUnlockButton : public button::Button {
 public:
  AutotermUART *parent_{nullptr};
  void set_parent(AutotermUART *p) { parent_ = p; }
 protected:
  void press_action() override;
};

class AutotermPrimePumpButton : public button::Button {
 public:
  AutotermUART *parent_{nullptr};
  uint8_t prime_frequency_{1};  // Hz
  void set_parent(AutotermUART *p) { parent_ = p; }
  void set_frequency(uint8_t freq) { prime_frequency_ = freq; }
 protected:
  void press_action() override;
};

// Status Report Button: requests all diagnostic data and formats into text sensor
class AutotermStatusReportButton : public button::Button {
 public:
  AutotermUART *parent_{nullptr};
  void set_parent(AutotermUART *p) { parent_ = p; }
 protected:
  void press_action() override;
};

// ===================
// Main class UART
// ===================
class AutotermUART : public Component {
  friend class AutotermTempSourceSelect;

 public:
  uart::UARTComponent *uart_display_{nullptr};
  uart::UARTComponent *uart_heater_{nullptr};

  // Sensors
  sensor::Sensor *internal_temp_sensor_{nullptr};
  sensor::Sensor *external_temp_sensor_{nullptr};
  sensor::Sensor *heater_temp_sensor_{nullptr};
  sensor::Sensor *panel_temp_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *status_sensor_{nullptr};
  sensor::Sensor *fan_speed_set_sensor_{nullptr};
  sensor::Sensor *fan_speed_actual_sensor_{nullptr};
  sensor::Sensor *pump_frequency_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  sensor::Sensor *panel_temp_override_sensor_{nullptr};
  float panel_temp_override_value_c_{NAN};
  AutotermTempSourceSelect *temp_source_select_{nullptr};
  bool manual_temp_source_active_{false};
  uint8_t manual_temp_source_value_{0};
  float last_internal_temp_c_{NAN};
  float last_external_temp_c_{NAN};


  AutotermFanLevelNumber *fan_level_number_{nullptr};
  AutotermClimate *climate_{nullptr};
  sensor::Sensor *runtime_hours_sensor_{nullptr};
  sensor::Sensor *session_runtime_sensor_{nullptr};
  ESPPreferenceObject runtime_hours_pref_;
  float runtime_hours_{0.0f};
  float runtime_hours_last_published_{NAN};
  float session_runtime_hours_{0.0f};
  float session_runtime_last_published_{NAN};
  bool runtime_loaded_{false};
  bool runtime_dirty_{false};
  bool runtime_tracking_initialized_{false};
  bool runtime_storage_initialized_{false};
  bool heater_running_{false};
  uint32_t last_runtime_millis_{0};
  uint32_t last_runtime_save_millis_{0};

  struct Settings {
    uint8_t use_work_time = 1;
    uint8_t work_time = 0;
    uint8_t temperature_source = 4;
    uint8_t set_temperature = 16;
    uint8_t wait_mode = 0;
    uint8_t power_level = 8;
  } settings_;
  bool settings_valid_{false};
  bool display_connected_state_{false};
  uint32_t last_display_activity_{0};
  uint32_t last_status_request_millis_{0};
  uint32_t last_settings_request_millis_{0};
  uint32_t last_panel_temp_send_millis_{0};
  float panel_temp_last_value_c_{NAN};
  std::vector<uint8_t> display_to_heater_buffer_;
  std::vector<uint8_t> heater_to_display_buffer_;
  bool thermostat_active_{false};
  bool thermostat_heating_request_{false};
  bool thermostat_waiting_for_idle_{false};
  float thermostat_target_c_{20.0f};
  float thermostat_hys_on_c_{2.0f};
  float thermostat_hys_off_c_{1.0f};

  // Persistent strings for traits to avoid dangling pointers
  std::vector<std::string> preset_strings_;
  std::vector<std::string> fan_mode_strings_;
  uint8_t thermostat_level_{4};
  uint8_t thermostat_sensor_source_{1};
  uint8_t thermostat_last_sent_level_{255};
  uint32_t thermostat_last_command_millis_{0};
  uint32_t thermostat_last_evaluation_millis_{0};

  // PID controller state
  bool pid_mode_active_{false};
  float pid_kp_{PID_KP_DEFAULT};
  float pid_ki_{PID_KI_DEFAULT};
  float pid_kd_{PID_KD_DEFAULT};
  float pid_integral_{0.0f};
  float pid_last_error_{0.0f};
  uint32_t pid_last_eval_ms_{0};
  PIDPhase pid_phase_{PID_PHASE_STARTUP};
  uint32_t pid_phase_start_ms_{0};

  // CRC Error Monitoring
  uint32_t crc_error_count_{0};
  uint32_t frame_count_{0};
  uint32_t crc_resync_count_{0};

  // UART loss detection
  uint32_t last_heater_activity_{0};
  bool heater_connected_{false};

  // Rate limiting
  uint32_t last_command_millis_{0};

  // Frame timeout
  uint32_t display_frame_start_millis_{0};
  uint32_t heater_frame_start_millis_{0};

  // Fuel consumption estimation
  sensor::Sensor *fuel_consumption_sensor_{nullptr};
  sensor::Sensor *total_fuel_sensor_{nullptr};
  float fuel_consumption_lph_{0.0f};
  float fuel_consumption_last_published_{NAN};
  float total_fuel_liters_{0.0f};
  float total_fuel_last_published_{NAN};
  ESPPreferenceObject fuel_pref_;
  bool fuel_dirty_{false};

  // Daily fuel tracking (resets at midnight)
  float daily_fuel_liters_{0.0f};
  float daily_fuel_last_published_{NAN};
  int last_fuel_reset_day_{-1};
  sensor::Sensor *daily_fuel_sensor_{nullptr};
  uint32_t last_fuel_update_ms_{0};

  // Combustion efficiency tracking
  sensor::Sensor *combustion_efficiency_sensor_{nullptr};
  sensor::Sensor *delta_t_sensor_{nullptr};
  float last_ambient_temp_c_{NAN};
  float last_exhaust_temp_c_{NAN};
  float combustion_efficiency_pct_{0.0f};
  float delta_t_c_{0.0f};

  // Ignition time tracking
  uint32_t ignition_start_ms_{0};
  bool ignition_tracking_active_{false};
  sensor::Sensor *ignition_time_sensor_{nullptr};
  float last_ignition_time_s_{NAN};

  // Maintenance reminders
  sensor::Sensor *maintenance_oil_sensor_{nullptr};
  sensor::Sensor *maintenance_filter_sensor_{nullptr};
  sensor::Sensor *maintenance_glow_sensor_{nullptr};
  bool maintenance_alert_oil_{false};
  bool maintenance_alert_filter_{false};
  bool maintenance_alert_glow_{false};

  // Frost protection
  bool frost_protection_active_{false};
  float frost_protection_temp_c_{FROST_PROTECTION_TEMP_C};

  // Night mode
  bool night_mode_active_{false};
  uint8_t night_mode_max_level_{3};

  // Advanced statistics
  uint32_t total_start_count_{0};
  uint32_t total_ignition_failures_{0};
  ESPPreferenceObject stats_pref_;
  bool stats_dirty_{false};

  // Software watchdog: loop health monitoring
  uint32_t loop_count_{0};
  uint32_t last_loop_watchdog_ms_{0};
  uint32_t loop_interval_max_ms_{0};
  uint32_t loop_interval_avg_ms_{0};
  uint32_t loop_interval_count_{0};

  // Boot diagnostics
  sensor::Sensor *boot_count_sensor_{nullptr};
  sensor::Sensor *free_heap_sensor_{nullptr};
  text_sensor::TextSensor *reset_reason_sensor_{nullptr};
  uint32_t boot_count_{0};
  ESPPreferenceObject boot_pref_;

  // Wear curve tracking (exhaust temp vs pump frequency)
  sensor::Sensor *wear_score_sensor_{nullptr};
  float wear_baseline_ratio_{0.0f};  // Expected ratio at factory (heater_temp / pump_freq)
  uint32_t wear_sample_count_{0};
  float wear_sum_ratios_{0.0f};
  bool wear_baseline_set_{false};

  // Fuel economy mode
  bool fuel_economy_active_{false};
  sensor::Sensor *fuel_economy_savings_sensor_{nullptr};
  float fuel_economy_savings_pct_{0.0f};
  uint32_t fuel_economy_active_ms_{0};

  // Configurable maintenance thresholds (overridable via YAML)
  float maintenance_oil_hrs_{MAINTENANCE_OIL_HOURS};
  float maintenance_filter_hrs_{MAINTENANCE_FILTER_HOURS};
  float maintenance_glow_hrs_{MAINTENANCE_GLOW_HOURS};

  // Intelligent prediction: temperature patterns by hour of day
  bool prediction_active_{false};
  float prediction_temps_[24];       // Average temp per hour (0-23)
  uint32_t prediction_counts_[24];   // Samples per hour
  bool prediction_initialized_{false};
  sensor::Sensor *predicted_temp_sensor_{nullptr};

  // Light sleep optimization
  bool light_sleep_enabled_{false};
  uint32_t last_uart_activity_ms_{0};

  // Active fuel economy: reduce power when near target
  bool fuel_economy_reactive_{false};
  uint8_t economy_reduced_level_{0};
  uint32_t economy_reduction_start_ms_{0};

  // Eco-Adaptive mode: continuous power modulation without on/off cycling
  bool eco_adaptive_active_{false};
  bool eco_adaptive_configured_{false};  // Set from YAML at startup, used to restore on re-enable
  float eco_kp_{1.5f};
  float eco_ki_{0.3f};
  float eco_kd_{0.2f};
  uint8_t eco_min_level_{1};
  uint8_t eco_max_level_{9};
  float eco_deadband_{0.3f};
  bool eco_overshoot_predict_{true};
  float eco_integral_{0.0f};
  float eco_last_error_{0.0f};
  float eco_last_temp_{NAN};
  uint32_t eco_last_eval_ms_{0};
  uint8_t eco_current_level_{0};
  uint32_t eco_last_command_ms_{0};
  sensor::Sensor *eco_adaptive_level_sensor_{nullptr};
  sensor::Sensor *eco_adaptive_error_sensor_{nullptr};
  sensor::Sensor *eco_power_efficiency_sensor_{nullptr};
  text_sensor::TextSensor *eco_mode_status_sensor_{nullptr};

  // PID output smoothing (circular buffer of last 3 outputs)
  float pid_output_history_[3] = {0.0f, 0.0f, 0.0f};
  uint8_t pid_output_index_{0};
  bool pid_output_initialized_{false};

  // Safety: emergency shutdown
  bool emergency_shutdown_active_{false};
  uint32_t emergency_start_ms_{0};

  // Safety: flameout detection
  uint32_t pump_active_since_ms_{0};
  bool pump_was_active_{false};

  // Safety: startup voltage check
  bool startup_voltage_ok_{true};

  // Burn-out protection: prevent shutdown during first 4 minutes after ignition
  bool burnout_protection_active_{false};
  uint32_t burnout_start_ms_{0};
  static constexpr uint32_t BURNOUT_PROTECTION_MS = 240000;  // 4 minutes

  // Safety: flameout confirmation counter (requires 2 consecutive low-temp readings)
  uint8_t flameout_confirm_count_{0};

  // Safety: voltage dip confirmation counter (requires 2 consecutive low-voltage readings)
  uint8_t voltage_dip_confirm_count_{0};

  // CO sensor: carbon monoxide detection (SAVES LIVES)
  sensor::Sensor *co_sensor_{nullptr};
  float last_co_ppm_{0.0f};
  uint8_t co_confirm_count_{0};
  static constexpr float CO_DANGER_PPM = 35.0f;  // Emergency shutdown above this
  static constexpr uint8_t CO_CONFIRM_REQUIRED = 3;  // Require 3 consecutive readings

  // Cached values for safety checks (updated from parse_status)
  float last_heater_temp_c_{NAN};
  float last_pump_freq_c_{0.0f};

  // Extended protocol: Diagnostic mode (0x07)
  bool diagnostic_mode_active_{false};
  sensor::Sensor *glow_plug_current_sensor_{nullptr};
  sensor::Sensor *chamber_temp_sensor_{nullptr};
  sensor::Sensor *board_temp_sensor_{nullptr};

  // Extended protocol: Error code from status (byte 2)
  uint8_t last_error_code_{0};
  sensor::Sensor *error_code_sensor_{nullptr};
  text_sensor::TextSensor *error_text_sensor_{nullptr};

  // Extended protocol: Version (0x06)
  text_sensor::TextSensor *firmware_version_sensor_{nullptr};

  // Extended protocol: History / Report (0x0B)
  uint16_t history_total_hours_{0};
  uint16_t history_total_starts_{0};
  uint8_t history_errors_[3]{0, 0, 0};
  sensor::Sensor *total_starts_sensor_{nullptr};
  text_sensor::TextSensor *error_log_sensor_{nullptr};

  // External temperature fallback cache
  float last_external_temp_cached_{NAN};
  uint32_t last_external_temp_update_ms_{0};

  void set_uart_display(uart::UARTComponent *u) { uart_display_ = u; }
  void set_uart_heater(uart::UARTComponent *u) { uart_heater_ = u; }

  // Sensor-Setter
  void set_internal_temp_sensor(sensor::Sensor *s) { internal_temp_sensor_ = s; }
  void set_external_temp_sensor(sensor::Sensor *s) { external_temp_sensor_ = s; }
  void set_heater_temp_sensor(sensor::Sensor *s) { heater_temp_sensor_ = s; }
  void set_voltage_sensor(sensor::Sensor *s) { voltage_sensor_ = s; }
  void set_status_sensor(sensor::Sensor *s) { status_sensor_ = s; }
  void set_fan_speed_set_sensor(sensor::Sensor *s) { fan_speed_set_sensor_ = s; }
  void set_fan_speed_actual_sensor(sensor::Sensor *s) { fan_speed_actual_sensor_ = s; }
  void set_pump_frequency_sensor(sensor::Sensor *s) { pump_frequency_sensor_ = s; }
  void set_panel_temp_sensor(sensor::Sensor *s) {
    panel_temp_sensor_ = s;
    if (s != nullptr && std::isfinite(panel_temp_last_value_c_)) {
      s->publish_state(panel_temp_last_value_c_);
    }
  }
  void set_status_text_sensor(text_sensor::TextSensor *s) { status_text_sensor_ = s; }
  void set_runtime_hours_sensor(sensor::Sensor *s);
  void set_session_runtime_sensor(sensor::Sensor *s);

  void set_panel_temp_override_sensor(sensor::Sensor *s);
  void set_temp_source_select(AutotermTempSourceSelect *select);
  void set_fuel_consumption_sensor(sensor::Sensor *s) { fuel_consumption_sensor_ = s; }
  void set_total_fuel_sensor(sensor::Sensor *s) { total_fuel_sensor_ = s; }
  void set_daily_fuel_sensor(sensor::Sensor *s) { daily_fuel_sensor_ = s; }
  void set_combustion_efficiency_sensor(sensor::Sensor *s) { combustion_efficiency_sensor_ = s; }
  void set_delta_t_sensor(sensor::Sensor *s) { delta_t_sensor_ = s; }
  void set_ignition_time_sensor(sensor::Sensor *s) { ignition_time_sensor_ = s; }
  void set_co_sensor(sensor::Sensor *s) { co_sensor_ = s; }
  void set_boot_count_sensor(sensor::Sensor *s) { boot_count_sensor_ = s; }
  void set_free_heap_sensor(sensor::Sensor *s) { free_heap_sensor_ = s; }
  void set_reset_reason_sensor(text_sensor::TextSensor *s) { reset_reason_sensor_ = s; }
  void set_wear_score_sensor(sensor::Sensor *s) { wear_score_sensor_ = s; }
  void set_fuel_economy_savings_sensor(sensor::Sensor *s) { fuel_economy_savings_sensor_ = s; }
  void set_maintenance_oil_hrs(float hrs) { maintenance_oil_hrs_ = hrs; }
  void set_maintenance_filter_hrs(float hrs) { maintenance_filter_hrs_ = hrs; }
  void set_maintenance_glow_hrs(float hrs) { maintenance_glow_hrs_ = hrs; }
  // Extended protocol sensor setters
  void set_glow_plug_current_sensor(sensor::Sensor *s) { glow_plug_current_sensor_ = s; }
  void set_chamber_temp_sensor(sensor::Sensor *s) { chamber_temp_sensor_ = s; }
  void set_board_temp_sensor(sensor::Sensor *s) { board_temp_sensor_ = s; }
  void set_error_code_sensor(sensor::Sensor *s) { error_code_sensor_ = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s) { error_text_sensor_ = s; }
  void set_firmware_version_sensor(text_sensor::TextSensor *s) { firmware_version_sensor_ = s; }
  void set_total_starts_sensor(sensor::Sensor *s) { total_starts_sensor_ = s; }
  void set_error_log_sensor(text_sensor::TextSensor *s) { error_log_sensor_ = s; }
  void enable_diagnostic_mode(bool enabled) { diagnostic_mode_active_ = enabled; }
  void enable_fuel_economy(bool enabled) { fuel_economy_active_ = enabled; }
  // Eco-Adaptive mode setters
  void enable_eco_adaptive(bool enabled) { eco_adaptive_active_ = enabled; eco_adaptive_configured_ = enabled; }
  void set_eco_gains(float kp, float ki, float kd) { eco_kp_ = kp; eco_ki_ = ki; eco_kd_ = kd; }
  void set_eco_levels(uint8_t min_l, uint8_t max_l) { eco_min_level_ = min_l; eco_max_level_ = max_l; }
  void set_eco_deadband(float db) { eco_deadband_ = db; }
  void enable_eco_overshoot_predict(bool enabled) { eco_overshoot_predict_ = enabled; }
  void set_eco_adaptive_level_sensor(sensor::Sensor *s) { eco_adaptive_level_sensor_ = s; }
  void set_eco_adaptive_error_sensor(sensor::Sensor *s) { eco_adaptive_error_sensor_ = s; }
  void set_eco_power_efficiency_sensor(sensor::Sensor *s) { eco_power_efficiency_sensor_ = s; }
  void set_eco_mode_status_sensor(text_sensor::TextSensor *s) { eco_mode_status_sensor_ = s; }
  void enable_fuel_economy_reactive(bool enabled) { fuel_economy_reactive_ = enabled; }
  void enable_prediction(bool enabled) { prediction_active_ = enabled; }
  void enable_light_sleep(bool enabled) { light_sleep_enabled_ = enabled; }
  void set_predicted_temp_sensor(sensor::Sensor *s) { predicted_temp_sensor_ = s; }
  void set_maintenance_oil_sensor(sensor::Sensor *s) { maintenance_oil_sensor_ = s; }
  void set_maintenance_filter_sensor(sensor::Sensor *s) { maintenance_filter_sensor_ = s; }
  void set_maintenance_glow_sensor(sensor::Sensor *s) { maintenance_glow_sensor_ = s; }
  void set_frost_protection_temp(float temp) { frost_protection_temp_c_ = temp; }
  void set_night_mode_max_level(uint8_t level) { night_mode_max_level_ = std::min<uint8_t>(level, 9); }
  void enable_night_mode(bool enabled) { night_mode_active_ = enabled; }
  void enable_frost_protection(bool enabled) { frost_protection_active_ = enabled; }
  void set_pid_gains(float kp, float ki, float kd) { pid_kp_ = kp; pid_ki_ = ki; pid_kd_ = kd; }
  void enable_pid_mode(bool enabled) { pid_mode_active_ = enabled; }
  void set_temp_source_from_select(uint8_t source);
  void apply_temp_source_from_settings(uint8_t source);
  uint8_t get_manual_temp_source() const { return manual_temp_source_active_ ? manual_temp_source_value_ : 0; }
  uint8_t get_effective_temp_source() const;
  float get_temperature_for_source(uint8_t source) const;

  // Setters with back reference
  void set_fan_level_number(AutotermFanLevelNumber *n) {
    fan_level_number_ = n;
    if (n) n->setup_parent(this);
  }
  void set_climate(AutotermClimate *climate);

  // Commands for operating modes
  void send_standby();
  void send_power_mode(bool start, uint8_t level);
  void send_temperature_hold_mode(bool start, uint8_t temp_sensor, uint8_t set_temp);
  void send_temperature_to_fan_mode(bool start, uint8_t temp_sensor, uint8_t set_temp);
  void send_fan_only(uint8_t level);
  void configure_thermostat_mode(float target_c, uint8_t level, uint8_t sensor_source,
                                 float hys_on_c, float hys_off_c);
  void disable_thermostat_mode();
  uint8_t compute_pid_output_(float current_temp, float target_temp, uint32_t now);

  // Extended protocol commands
  void send_diagnostic_mode_(bool enable);
  void send_unlock_();
  void send_report_request_();
  void send_version_request_();
  void send_prime_pump_(uint8_t frequency);
  void send_handshake_();
  void parse_diagnostic_(const std::vector<uint8_t> &data);
  void parse_version_(const std::vector<uint8_t> &data);
  void parse_history_(const std::vector<uint8_t> &data);
  static const char *error_code_to_text_(uint8_t code);

  void loop() override {
    forward_and_sniff(uart_display_, uart_heater_, "display→heater", true);
    forward_and_sniff(uart_heater_, uart_display_, "heater→display");

    uint32_t now = millis();
    bool connected = uart_display_ != nullptr && (now - last_display_activity_) < DISPLAY_TIMEOUT_MS;
    if (connected != display_connected_state_) {
      display_connected_state_ = connected;
      if (connected) {
        ESP_LOGI("autoterm_uart", "Display connection detected");
        last_status_request_millis_ = now;
        last_settings_request_millis_ = now;
        last_panel_temp_send_millis_ = now;
      } else {
        ESP_LOGW("autoterm_uart", "Display connection lost, switching to autonomous mode");
        last_panel_temp_send_millis_ = 0;
      }
    }

    // Adaptive polling: faster when active, slower when idle
    uint32_t status_interval = heater_running_ ? STATUS_REQUEST_INTERVAL_MS : STATUS_REQUEST_INTERVAL_MS * 3;
    uint32_t settings_interval = heater_running_ ? SETTINGS_REQUEST_INTERVAL_MS : SETTINGS_REQUEST_INTERVAL_MS * 2;

    if (!connected) {
      if (now - last_status_request_millis_ >= status_interval) {
        send_status_request();
        last_status_request_millis_ = now;
      }
      if (now - last_settings_request_millis_ >= settings_interval) {
        request_settings();
        last_settings_request_millis_ = now;
      }
      if (should_override_panel_temperature_() && std::isfinite(panel_temp_override_value_c_)) {
        if (last_panel_temp_send_millis_ == 0 || (now - last_panel_temp_send_millis_) >= PANEL_TEMP_INTERVAL_MS) {
          send_panel_temperature_override_frame_();
          last_panel_temp_send_millis_ = now;
        }
      }
    }

    uint32_t runtime_now = millis();
    advance_runtime_time_(runtime_now);
    maybe_save_runtime_hours_(runtime_now);
    maybe_save_fuel_(runtime_now);

    // Check heater UART connection
    check_uart_loss_(now);

    // Safety checks (run on every loop)
    // Burn-out protection suppresses flameout check during first 4 min
    if (heater_running_ && !emergency_shutdown_active_ && !burnout_protection_active_) {
      check_flameout_(last_heater_temp_c_, last_pump_freq_c_, now);
    }

    // CO sensor check (SAVES LIVES — runs on every loop)
    check_co_level_();

    // Emergency recovery check (auto-recover after 5 minutes if safe)
    check_emergency_recovery_(now);

    if (thermostat_active_ || eco_adaptive_active_)
      evaluate_thermostat_control_();

    // Frost protection check (every 30 seconds)
    static uint32_t last_frost_check = 0;
    if (frost_protection_active_ && (now - last_frost_check) > 30000) {
      evaluate_frost_protection_();
      last_frost_check = now;
    }

    // Extended protocol: enable diagnostic mode after 5 seconds uptime (once)
    static bool diagnostic_sent = false;
    if (diagnostic_mode_active_ && !diagnostic_sent && now > 5000) {
      send_diagnostic_mode_(true);
      diagnostic_sent = true;
    }

    // Request history/report every 5 minutes
    static uint32_t last_report_request = 0;
    if ((now - last_report_request) > 300000) {
      send_report_request_();
      last_report_request = now;
    }

    // Maintenance check (every 5 minutes)
    static uint32_t last_maintenance_check = 0;
    if ((now - last_maintenance_check) > 300000) {
      check_maintenance_();
      publish_maintenance_();
      last_maintenance_check = now;
    }

    // Software watchdog: track loop health
    update_loop_watchdog_(now);

    // Fuel economy tracking
    update_fuel_economy_savings_(now);

    // System health monitoring (every 60s)
    update_system_health_(now);

    // Active fuel economy: reduce power when near target
    evaluate_fuel_economy_reactive_(now);

    // Light sleep evaluation
    evaluate_light_sleep_(now);

    // Periodic backup of all persistent data
    periodic_backup_(now);

    // System health log (every 15 minutes)
    static uint32_t last_health_log = 0;
    if ((now - last_health_log) > 900000) {
      last_health_log = now;
      publish_system_health_();
    }
  }

  void setup() override {
    if (global_preferences != nullptr) {
      runtime_hours_pref_ =
          global_preferences->make_preference<float>(fnv1_hash("autoterm_uart_runtime_hours"));
      runtime_storage_initialized_ = true;
      if (!runtime_hours_pref_.load(&runtime_hours_)) {
        runtime_hours_ = 0.0f;
      }
    } else {
      runtime_storage_initialized_ = false;
      runtime_hours_ = 0.0f;
    }
    runtime_loaded_ = true;
    runtime_hours_last_published_ = NAN;
    publish_runtime_hours_(true);
    session_runtime_hours_ = 0.0f;
    session_runtime_last_published_ = NAN;
    publish_session_runtime_(true);

    uint32_t now = millis();
    last_runtime_millis_ = now;
    last_runtime_save_millis_ = now;
    last_heater_activity_ = now;
    runtime_tracking_initialized_ = true;

    // Load fuel and statistics data
    load_fuel_data_();
    load_stats_();
    load_boot_count_();

    ESP_LOGI("autoterm_uart", "Startup: %.1f runtime hours, %.2fL fuel consumed, %u starts, boot #%u",
             runtime_hours_, total_fuel_liters_, total_start_count_, boot_count_);

    // Publish boot diagnostics
    publish_boot_diagnostics_();

    // Initialize prediction arrays
    for (int i = 0; i < 24; i++) {
      prediction_temps_[i] = NAN;
      prediction_counts_[i] = 0;
    }

    request_settings();

    // Extended protocol: request firmware version at startup
    send_version_request_();

    // Extended protocol: enable diagnostic mode if configured
    if (diagnostic_mode_active_) {
      // Delayed enable to avoid overwhelming the UART during startup
      // Will be enabled in loop() after a short delay
    }
  }

 protected:
  void forward_and_sniff(uart::UARTComponent *src, uart::UARTComponent *dst, const char *tag,
                         bool from_display = false);
  bool validate_crc(const std::vector<uint8_t> &data);
  bool validate_frame_structure_(const std::vector<uint8_t> &frame);
  void log_frame(const char *tag, const std::vector<uint8_t> &data);

  void parse_status(const std::vector<uint8_t> &data);
  void parse_settings(const std::vector<uint8_t> &data, bool from_display);
public:
  void send_fan_mode(bool on, int level);

 protected:
  void request_settings();
  void send_status_request();
  void send_panel_temperature_override_frame_();
  bool is_panel_temperature_frame_(const std::vector<uint8_t> &frame) const;
  void handle_panel_temperature_frame_(const std::vector<uint8_t> &frame);
  void process_frame_(std::vector<uint8_t> frame, uart::UARTComponent *dst, const char *tag, bool from_display);
  bool should_override_panel_temperature_() const;
  void apply_temp_source_override_(std::vector<uint8_t> &frame);
  uint8_t compute_override_temperature_byte_() const;
  void update_crc_(std::vector<uint8_t> &frame);
  bool send_command_(uint8_t command, const std::vector<uint8_t> &payload, const char *log_label);
  uint16_t append_crc_(std::vector<uint8_t> &frame);
  static uint16_t crc16_modbus_(const uint8_t *data, size_t length);
  void evaluate_thermostat_control_(bool force = false);
  void evaluate_eco_adaptive_(bool force = false);
  void update_system_health_(uint32_t now);
  void handle_thermostat_status_update_(uint16_t status_code);
  void send_thermostat_cooldown_(uint8_t source, uint8_t temp_byte);
  float clamp_thermostat_target_(float target) const;
  float clamp_thermostat_hys_on_(float value) const;
  float clamp_thermostat_hys_off_(float value) const;

  void publish_temp_source_select_(uint8_t source);
  uint8_t clamp_temp_source_(uint8_t source) const;
  bool should_force_temp_source_() const;
  uint8_t map_source_to_heater_(uint8_t source) const;
  void advance_runtime_time_(uint32_t now);
  void set_heater_running_state_(bool running);
  void publish_runtime_hours_(bool force = false);
  void publish_session_runtime_(bool force = false);
  void maybe_save_runtime_hours_(uint32_t now, bool force = false);
  bool is_heater_active_status_(uint16_t status_code) const;

  // New: Reliability
  void resync_uart_buffer_(std::vector<uint8_t> &buffer);
  bool check_frame_timeout_(uint32_t frame_start, uint32_t now) const;
  void check_uart_loss_(uint32_t now);

  // New: Rate limiting
  bool check_command_rate_limit_(uint32_t now);

  // New: Fuel consumption
  void update_fuel_consumption_(float pump_freq);
  void publish_fuel_consumption_(bool force = false);
  void publish_total_fuel_(bool force = false);
  void maybe_save_fuel_(uint32_t now, bool force = false);
  void load_fuel_data_();

  // New: Combustion efficiency
  void update_combustion_efficiency_(float heater_temp, float ambient_temp);
  void track_ignition_time_(uint16_t status_code, uint32_t now);

  // New: Software watchdog & diagnostics
  void update_loop_watchdog_(uint32_t now);
  void publish_boot_diagnostics_();
  void load_boot_count_();
  static const char* get_reset_reason_str_(esp_reset_reason_t reason);

  // New: Wear curve tracking
  void update_wear_score_(float heater_temp, float pump_freq);

  // New: Fuel economy
  void update_fuel_economy_savings_(uint32_t now);

  // New: Intelligent prediction
  void update_prediction_(float temp, uint32_t now);
  void publish_predicted_temp_();
  void load_prediction_data_();
  void save_prediction_data_();

  // New: Light sleep
  void evaluate_light_sleep_(uint32_t now);

  // New: Active fuel economy
  void evaluate_fuel_economy_reactive_(uint32_t now);

  // New: Periodic backup of all persistent data
  void periodic_backup_(uint32_t now);

  // New: System health summary
  void publish_system_health_();

  // New: Startup phase monitor
  void monitor_startup_phase_(uint16_t status_code, float voltage, float heater_temp, uint32_t now);
  uint16_t last_startup_status_{0};
  uint32_t startup_phase_start_ms_{0};
  bool startup_monitoring_active_{false};

  // New: Maintenance
  void check_maintenance_();
  void publish_maintenance_();

  // New: Frost protection
  void evaluate_frost_protection_();

  // New: Safety
  void check_emergency_shutdown_(float heater_temp, float voltage, uint16_t status_code);
  void check_emergency_recovery_(uint32_t now);
  void check_flameout_(float heater_temp, float pump_freq, uint32_t now);
  void check_startup_voltage_(float voltage, uint16_t status_code);
  void check_co_level_();
  void emergency_stop_(const char *reason);

  // New: Statistics
  void load_stats_();
  void save_stats_();
  void increment_start_count_();
};

// ===================
// Climate-Class
// ===================
class AutotermClimate : public climate::Climate {
 public:
 void set_parent(AutotermUART *parent);
 void set_default_level(uint8_t level);
 void set_default_temperature(float temperature_c);
 void set_default_temp_sensor(uint8_t sensor);
  void set_thermostat_hysteresis(float hys_on_c, float hys_off_c);

  void handle_status_update(uint16_t status_code, float internal_temp);
  void handle_settings_update(const AutotermUART::Settings &settings, bool from_display);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 private:
 AutotermUART *parent_{nullptr};
 float target_temperature_c_{20.0f};
 float current_temperature_c_{NAN};
 uint8_t fan_level_{4};
  uint8_t default_temp_sensor_{0x01};
  std::string preset_mode_{"power_mode"};
  float thermostat_hys_on_c_{2.0f};
  float thermostat_hys_off_c_{1.0f};

  // NEW: Persistent strings for traits to avoid dangling pointers
  std::vector<std::string> preset_strings_;
  std::vector<std::string> fan_mode_strings_;

  static uint8_t clamp_level_(int level);
  static float clamp_temperature_(float temperature);
  static float clamp_hysteresis_on_(float value);
  static float clamp_hysteresis_off_(float value);
  std::string fan_mode_label_from_level_(uint8_t level) const;
  uint8_t fan_mode_label_to_level_(const std::string &label) const;
  std::string sanitize_preset_(const std::string &preset) const;
  uint8_t resolve_temp_sensor_() const;
  climate::ClimateMode deduce_mode_from_settings_(const AutotermUART::Settings &settings) const;
  std::string deduce_preset_from_settings_(const AutotermUART::Settings &settings) const;
  void apply_state_(climate::ClimateMode mode, const std::string &preset, uint8_t level, float target_temp);
  void update_action_from_status_(uint16_t status_code);
  static std::string preset_from_enum_(climate::ClimatePreset preset);
  static uint8_t fan_level_from_enum_(climate::ClimateFanMode mode, uint8_t fallback_level);
};

}  // namespace autoterm_uart
}  // namespace esphome
