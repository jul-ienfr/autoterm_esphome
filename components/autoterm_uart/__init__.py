import esphome.config_validation as cv
import esphome.codegen as cg
from esphome import const
import esphome.components.uart as uart
import esphome.components.sensor as sensor
import esphome.components.text_sensor as text_sensor
import esphome.components.number as number
import esphome.components.climate as climate
import esphome.components.select as select
import esphome.components.button as button

__version__ = "1.0.0"

DEPENDENCIES = ["sensor", "text_sensor", "number", "climate"]
AUTO_LOAD = ["sensor", "text_sensor", "number", "climate", "select", "button"]

autoterm_ns = cg.esphome_ns.namespace("autoterm_uart")
AutotermFanLevelNumber = autoterm_ns.class_("AutotermFanLevelNumber", number.Number)
AutotermUART = autoterm_ns.class_("AutotermUART", cg.Component)
AutotermClimate = autoterm_ns.class_("AutotermClimate", climate.Climate)
AutotermTempSourceSelect = autoterm_ns.class_("AutotermTempSourceSelect", select.Select)
AutotermUnlockButton = autoterm_ns.class_("AutotermUnlockButton", button.Button)
AutotermPrimePumpButton = autoterm_ns.class_("AutotermPrimePumpButton", button.Button)
AutotermStatusReportButton = autoterm_ns.class_("AutotermStatusReportButton", button.Button)

CONF_CLIMATE = "climate"
CONF_DEFAULT_LEVEL = "default_level"
CONF_DEFAULT_TEMPERATURE = "default_temperature"
CONF_DEFAULT_TEMP_SENSOR = "default_temp_sensor"
CONF_THERMOSTAT_HYS_ON = "thermostat_hysteresis_on"
CONF_THERMOSTAT_HYS_OFF = "thermostat_hysteresis_off"
CONF_PANEL_TEMP_OVERRIDE = "panel_temp_override"
CONF_PANEL_TEMP_OVERRIDE_SENSOR = "sensor"
CONF_TEMP_SOURCE_SELECT = "temperature_source_select"
CONF_FUEL_CONSUMPTION = "fuel_consumption"
CONF_MAINTENANCE_OIL = "maintenance_oil"
CONF_MAINTENANCE_FILTER = "maintenance_filter"
CONF_MAINTENANCE_GLOW = "maintenance_glow"
CONF_FROST_PROTECTION = "frost_protection"
CONF_FROST_PROTECTION_TEMP = "frost_protection_temp"
CONF_NIGHT_MODE = "night_mode"
CONF_NIGHT_MODE_MAX_LEVEL = "night_mode_max_level"
CONF_PID_MODE = "pid_mode"
CONF_PID_KP = "pid_kp"
CONF_PID_KI = "pid_ki"
CONF_PID_KD = "pid_kd"
CONF_MAINTENANCE_OIL_HRS = "maintenance_oil_hours"
CONF_MAINTENANCE_FILTER_HRS = "maintenance_filter_hours"
CONF_MAINTENANCE_GLOW_HRS = "maintenance_glow_hours"
CONF_BURN_CYCLE_INTERVAL_HRS = "burn_cycle_interval_hours"
CONF_FUEL_ECONOMY = "fuel_economy"
CONF_FUEL_ECONOMY_REACTIVE = "fuel_economy_reactive"
CONF_PREDICTION = "prediction"
CONF_LIGHT_SLEEP = "light_sleep"
CONF_DIAGNOSTIC_MODE = "diagnostic_mode"
CONF_UNLOCK_BUTTON = "unlock_button"
CONF_PRIME_PUMP_BUTTON = "prime_pump_button"
CONF_PRIME_PUMP_FREQUENCY = "prime_frequency"
CONF_STATUS_REPORT_BUTTON = "status_report_button"
CONF_ERROR_CODE = "error_code"
CONF_ERROR_TEXT = "error_text"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_TOTAL_STARTS = "total_starts"
CONF_ERROR_LOG = "error_log"
CONF_ECO_ADAPTIVE = "eco_adaptive"
CONF_ECO_KP = "eco_kp"
CONF_ECO_KI = "eco_ki"
CONF_ECO_KD = "eco_kd"
CONF_ECO_MIN_LEVEL = "eco_min_level"
CONF_ECO_MAX_LEVEL = "eco_max_level"
CONF_ECO_DEADBAND = "eco_deadband"
CONF_ECO_OVERSHOOT_PREDICT = "eco_overshoot_predict"
CONF_MAX_PUMP_FREQ = "max_pump_freq"
CONF_ECO_ADAPTIVE_LEVEL = "eco_adaptive_level"
CONF_ECO_ADAPTIVE_ERROR = "eco_adaptive_error"
CONF_ECO_POWER_EFFICIENCY = "eco_power_efficiency"
CONF_ECO_MODE_STATUS = "eco_mode_status"
CONF_GLOW_PLUG_CURRENT = "glow_plug_current"
CONF_CHAMBER_TEMP = "chamber_temp"
CONF_BOARD_TEMP = "board_temp"

TEMP_SOURCE_OPTIONS = ["Intern", "Panel", "Extern", "Home Assistant"]

CLIMATE_SCHEMA = climate.climate_schema(AutotermClimate).extend({
    cv.Optional(CONF_DEFAULT_LEVEL, default=4): cv.int_range(min=0, max=9),
    cv.Optional(CONF_DEFAULT_TEMPERATURE, default=20.0): cv.temperature,
    cv.Optional(CONF_DEFAULT_TEMP_SENSOR, default=2): cv.int_range(min=1, max=4),
    cv.Optional(CONF_THERMOSTAT_HYS_ON, default=2.0): cv.float_range(min=1.0, max=5.0),
    cv.Optional(CONF_THERMOSTAT_HYS_OFF, default=1.0): cv.float_range(min=0.0, max=2.0),
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(AutotermUART),
    cv.Required("uart_display_id"): cv.use_id(uart.UARTComponent),
    cv.Required("uart_heater_id"): cv.use_id(uart.UARTComponent),

    cv.Optional("internal_temp"): sensor.sensor_schema(unit_of_measurement="°C", icon="mdi:thermometer"),
    cv.Optional("external_temp"): sensor.sensor_schema(unit_of_measurement="°C", icon="mdi:thermometer"),
    cv.Optional("heater_temp"): sensor.sensor_schema(unit_of_measurement="°C", icon="mdi:thermometer"),
    cv.Optional("panel_temp"): sensor.sensor_schema(unit_of_measurement="°C", icon="mdi:thermometer"),
    cv.Optional("voltage"): sensor.sensor_schema(unit_of_measurement="V", icon="mdi:flash"),
    cv.Optional("status"): sensor.sensor_schema(icon="mdi:information"),
    cv.Optional("fan_speed_set"): sensor.sensor_schema(unit_of_measurement="rpm", icon="mdi:fan"),
    cv.Optional("fan_speed_actual"): sensor.sensor_schema(unit_of_measurement="rpm", icon="mdi:fan"),
    cv.Optional("pump_frequency"): sensor.sensor_schema(unit_of_measurement="Hz", icon="mdi:water-pump"),
    cv.Optional("runtime_hours"): sensor.sensor_schema(
        unit_of_measurement="h",
        icon="mdi:clock-outline",
        accuracy_decimals=2,
        device_class=const.DEVICE_CLASS_DURATION,
        state_class=const.STATE_CLASS_TOTAL_INCREASING,
    ),
    cv.Optional("session_runtime"): sensor.sensor_schema(
        unit_of_measurement="h",
        icon="mdi:timer-outline",
        accuracy_decimals=2,
        device_class=const.DEVICE_CLASS_DURATION,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_FUEL_CONSUMPTION): sensor.sensor_schema(
        unit_of_measurement="L/h",
        icon="mdi:gas-station",
        accuracy_decimals=2,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("total_fuel_consumed"): sensor.sensor_schema(
        unit_of_measurement="L",
        icon="mdi:gas-station-outline",
        accuracy_decimals=2,
        state_class=const.STATE_CLASS_TOTAL_INCREASING,
        device_class=const.DEVICE_CLASS_GAS,
    ),
    cv.Optional("daily_fuel_consumed"): sensor.sensor_schema(
        unit_of_measurement="L",
        icon="mdi:gas-station",
        accuracy_decimals=2,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("combustion_efficiency"): sensor.sensor_schema(
        unit_of_measurement="%",
        icon="mdi:fire",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("delta_t"): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:thermometer-plus",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("ignition_time"): sensor.sensor_schema(
        unit_of_measurement="s",
        icon="mdi:timer-sand",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("boot_count"): sensor.sensor_schema(
        icon="mdi:counter",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_TOTAL_INCREASING,
    ),
    cv.Optional("free_heap"): sensor.sensor_schema(
        unit_of_measurement="B",
        icon="mdi:memory",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("reset_reason"): text_sensor.text_sensor_schema(
        icon="mdi:restart-alert",
    ),
    cv.Optional(CONF_MAINTENANCE_OIL): sensor.sensor_schema(
        icon="mdi:oil",
        device_class=const.DEVICE_CLASS_PROBLEM,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_MAINTENANCE_FILTER): sensor.sensor_schema(
        icon="mdi:air-filter",
        device_class=const.DEVICE_CLASS_PROBLEM,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_MAINTENANCE_GLOW): sensor.sensor_schema(
        icon="mdi:lightning-bolt",
        device_class=const.DEVICE_CLASS_PROBLEM,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),

    cv.Optional("status_text"): text_sensor.text_sensor_schema(icon="mdi:information"),

    cv.Optional("fan_level"): number.number_schema(class_=AutotermFanLevelNumber, icon="mdi:fan-speed-1"),

    cv.Optional(CONF_CLIMATE): CLIMATE_SCHEMA,
    cv.Optional(CONF_PANEL_TEMP_OVERRIDE): cv.Schema({
        cv.Required(CONF_PANEL_TEMP_OVERRIDE_SENSOR): cv.use_id(sensor.Sensor),
    }),
    cv.Optional(CONF_TEMP_SOURCE_SELECT): select.select_schema(class_=AutotermTempSourceSelect, icon="mdi:thermometer-probe"),
    cv.Optional(CONF_FROST_PROTECTION, default=False): cv.boolean,
    cv.Optional(CONF_FROST_PROTECTION_TEMP, default=2.0): cv.float_range(min=-10.0, max=10.0),
    cv.Optional(CONF_NIGHT_MODE, default=False): cv.boolean,
    cv.Optional(CONF_NIGHT_MODE_MAX_LEVEL, default=3): cv.int_range(min=0, max=9),

    # PID controller
    cv.Optional(CONF_PID_MODE, default=False): cv.boolean,
    cv.Optional(CONF_PID_KP, default=2.0): cv.float_range(min=0.1, max=10.0),
    cv.Optional(CONF_PID_KI, default=0.5): cv.float_range(min=0.0, max=5.0),
    cv.Optional(CONF_PID_KD, default=0.1): cv.float_range(min=0.0, max=2.0),

    # Eco-Adaptive mode
    cv.Optional(CONF_ECO_ADAPTIVE, default=False): cv.boolean,
    cv.Optional(CONF_ECO_KP, default=1.5): cv.float_range(min=0.1, max=10.0),
    cv.Optional(CONF_ECO_KI, default=0.3): cv.float_range(min=0.0, max=5.0),
    cv.Optional(CONF_ECO_KD, default=0.2): cv.float_range(min=0.0, max=2.0),
    cv.Optional(CONF_ECO_MIN_LEVEL, default=1): cv.int_range(min=1, max=9),
    cv.Optional(CONF_ECO_MAX_LEVEL, default=9): cv.int_range(min=1, max=9),
    cv.Optional(CONF_ECO_DEADBAND, default=0.3): cv.float_range(min=0.0, max=2.0),
    cv.Optional(CONF_ECO_OVERSHOOT_PREDICT, default=True): cv.boolean,
    cv.Optional(CONF_MAX_PUMP_FREQ, default=5.0): cv.float_range(min=1.0, max=10.0),
    cv.Optional(CONF_ECO_ADAPTIVE_LEVEL): sensor.sensor_schema(
        icon="mdi:thermostat",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_ECO_ADAPTIVE_ERROR): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:math-difference",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_ECO_POWER_EFFICIENCY): sensor.sensor_schema(
        unit_of_measurement="%",
        icon="mdi:leaf",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_ECO_MODE_STATUS): text_sensor.text_sensor_schema(
        icon="mdi:thermostat-auto",
    ),

    # Configurable maintenance thresholds
    cv.Optional(CONF_MAINTENANCE_OIL_HRS, default=500.0): cv.float_range(min=50.0, max=2000.0),
    cv.Optional(CONF_MAINTENANCE_FILTER_HRS, default=200.0): cv.float_range(min=50.0, max=1000.0),
    cv.Optional(CONF_MAINTENANCE_GLOW_HRS, default=1000.0): cv.float_range(min=100.0, max=3000.0),
    cv.Optional(CONF_BURN_CYCLE_INTERVAL_HRS, default=100.0): cv.float_range(min=10.0, max=500.0),

    # Fuel economy mode
    cv.Optional(CONF_FUEL_ECONOMY, default=False): cv.boolean,
    cv.Optional(CONF_FUEL_ECONOMY_REACTIVE, default=False): cv.boolean,

    # Intelligent prediction
    cv.Optional(CONF_PREDICTION, default=False): cv.boolean,

    # Light sleep
    cv.Optional(CONF_LIGHT_SLEEP, default=False): cv.boolean,

    # Wear tracking sensor
    cv.Optional("wear_score"): sensor.sensor_schema(
        unit_of_measurement="%",
        icon="mdi:gauge",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("fuel_economy_savings"): sensor.sensor_schema(
        unit_of_measurement="%",
        icon="mdi:leaf",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional("predicted_temp"): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:thermometer-auto",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),

    # Extended protocol: diagnostic sensors
    cv.Optional(CONF_GLOW_PLUG_CURRENT): sensor.sensor_schema(
        unit_of_measurement="%",
        icon="mdi:lightning-bolt",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_CHAMBER_TEMP): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:fire",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_BOARD_TEMP): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:thermometer",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),

    # CO sensor (SAVES LIVES — carbon monoxide detection)
    cv.Optional("co_level"): sensor.sensor_schema(
        unit_of_measurement="ppm",
        icon="mdi:smoke",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
        device_class=const.DEVICE_CLASS_CARBON_MONOXIDE,
    ),

    # GPS altitude (from HA Companion App — no hardware needed)
    cv.Optional("gps_altitude"): sensor.sensor_schema(
        unit_of_measurement="m",
        icon="mdi:map-marker海拔",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),

    # Exhaust temp direct probe (optional: Thermocouple K + MAX6675)
    # When present: more accurate than UART-derived exhaust temp
    # When absent: falls back to UART temperature (no hardware required)
    cv.Optional("exhaust_temp_direct"): sensor.sensor_schema(
        unit_of_measurement="°C",
        icon="mdi:thermometer",
        accuracy_decimals=1,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),

    # Extended protocol: error/history sensors
    cv.Optional(CONF_ERROR_CODE): sensor.sensor_schema(
        icon="mdi:alert-circle",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_MEASUREMENT,
    ),
    cv.Optional(CONF_ERROR_TEXT): text_sensor.text_sensor_schema(
        icon="mdi:alert",
    ),
    cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
        icon="mdi:information",
    ),
    cv.Optional(CONF_TOTAL_STARTS): sensor.sensor_schema(
        icon="mdi:counter",
        accuracy_decimals=0,
        state_class=const.STATE_CLASS_TOTAL_INCREASING,
    ),
    cv.Optional(CONF_ERROR_LOG): text_sensor.text_sensor_schema(
        icon="mdi:file-document-alert",
    ),

    # Extended protocol: buttons
    cv.Optional(CONF_UNLOCK_BUTTON): button.button_schema(
        class_=AutotermUnlockButton,
        icon="mdi:key-variant",
    ),
    cv.Optional(CONF_PRIME_PUMP_BUTTON): button.button_schema(
        class_=AutotermPrimePumpButton,
        icon="mdi:water-pump",
    ).extend({
        cv.Optional(CONF_PRIME_PUMP_FREQUENCY, default=1): cv.int_range(min=1, max=255),
    }),
    cv.Optional(CONF_STATUS_REPORT_BUTTON): button.button_schema(
        class_=AutotermStatusReportButton,
        icon="mdi:file-document-edit",
    ),

    # Extended protocol: diagnostic mode
    cv.Optional(CONF_DIAGNOSTIC_MODE, default=False): cv.boolean,

})


async def to_code(config):
    var = cg.new_Pvariable(config[const.CONF_ID])
    await cg.register_component(var, config)
    disp = await cg.get_variable(config["uart_display_id"])
    heat = await cg.get_variable(config["uart_heater_id"])
    cg.add(var.set_uart_display(disp))
    cg.add(var.set_uart_heater(heat))

    for key, setter in [
        ("internal_temp", "set_internal_temp_sensor"),
        ("external_temp", "set_external_temp_sensor"),
        ("heater_temp", "set_heater_temp_sensor"),
        ("panel_temp", "set_panel_temp_sensor"),
        ("voltage", "set_voltage_sensor"),
        ("status", "set_status_sensor"),
        ("fan_speed_set", "set_fan_speed_set_sensor"),
        ("fan_speed_actual", "set_fan_speed_actual_sensor"),
        ("pump_frequency", "set_pump_frequency_sensor"),
        ("runtime_hours", "set_runtime_hours_sensor"),
        ("session_runtime", "set_session_runtime_sensor"),
        (CONF_FUEL_CONSUMPTION, "set_fuel_consumption_sensor"),
        ("total_fuel_consumed", "set_total_fuel_sensor"),
        ("daily_fuel_consumed", "set_daily_fuel_sensor"),
        ("combustion_efficiency", "set_combustion_efficiency_sensor"),
        ("delta_t", "set_delta_t_sensor"),
        ("ignition_time", "set_ignition_time_sensor"),
        ("boot_count", "set_boot_count_sensor"),
        ("free_heap", "set_free_heap_sensor"),
        ("wear_score", "set_wear_score_sensor"),
        ("fuel_economy_savings", "set_fuel_economy_savings_sensor"),
        ("predicted_temp", "set_predicted_temp_sensor"),
        (CONF_MAINTENANCE_OIL, "set_maintenance_oil_sensor"),
        (CONF_MAINTENANCE_FILTER, "set_maintenance_filter_sensor"),
        (CONF_MAINTENANCE_GLOW, "set_maintenance_glow_sensor"),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    for key, setter in [
        ("status_text", "set_status_text_sensor"),
        ("reset_reason", "set_reset_reason_sensor"),
    ]:
        if key in config:
            txt = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(txt))

    if "fan_level" in config:
        conf = config["fan_level"]
        # Standardwerte definieren, falls nicht im YAML angegeben
        min_v = conf.get("min_value", 0)
        max_v = conf.get("max_value", 9)
        step_v = conf.get("step", 1)
        num = await number.new_number(conf, min_value=min_v, max_value=max_v, step=step_v)
        cg.add(var.set_fan_level_number(num))

    if CONF_CLIMATE in config:
        climate_conf = config[CONF_CLIMATE]
        clim = cg.new_Pvariable(climate_conf[const.CONF_ID])
       # await cg.register_component(clim, climate_conf)
        await climate.register_climate(clim, climate_conf)
        cg.add(clim.set_default_level(climate_conf[CONF_DEFAULT_LEVEL]))
        cg.add(clim.set_default_temperature(climate_conf[CONF_DEFAULT_TEMPERATURE]))
        cg.add(clim.set_default_temp_sensor(climate_conf[CONF_DEFAULT_TEMP_SENSOR]))
        cg.add(clim.set_thermostat_hysteresis(
            climate_conf[CONF_THERMOSTAT_HYS_ON],
            climate_conf[CONF_THERMOSTAT_HYS_OFF],
        ))
        cg.add(var.set_climate(clim))

    if CONF_PANEL_TEMP_OVERRIDE in config:
        override_conf = config[CONF_PANEL_TEMP_OVERRIDE]
        src = await cg.get_variable(override_conf[CONF_PANEL_TEMP_OVERRIDE_SENSOR])
        cg.add(var.set_panel_temp_override_sensor(src))

    if CONF_TEMP_SOURCE_SELECT in config:
        select_conf = config[CONF_TEMP_SOURCE_SELECT]
        sel = await select.new_select(select_conf, options=TEMP_SOURCE_OPTIONS)
        cg.add(var.set_temp_source_select(sel))

    # Frost protection
    if CONF_FROST_PROTECTION in config:
        cg.add(var.enable_frost_protection(config[CONF_FROST_PROTECTION]))
    if CONF_FROST_PROTECTION_TEMP in config:
        cg.add(var.set_frost_protection_temp(config[CONF_FROST_PROTECTION_TEMP]))

    # Night mode
    if CONF_NIGHT_MODE in config:
        cg.add(var.enable_night_mode(config[CONF_NIGHT_MODE]))
    if CONF_NIGHT_MODE_MAX_LEVEL in config:
        cg.add(var.set_night_mode_max_level(config[CONF_NIGHT_MODE_MAX_LEVEL]))

    # PID controller
    if CONF_PID_MODE in config:
        cg.add(var.enable_pid_mode(config[CONF_PID_MODE]))
    if CONF_PID_KP in config or CONF_PID_KI in config or CONF_PID_KD in config:
        kp = config.get(CONF_PID_KP, 2.0)
        ki = config.get(CONF_PID_KI, 0.5)
        kd = config.get(CONF_PID_KD, 0.1)
        cg.add(var.set_pid_gains(kp, ki, kd))

    # Configurable maintenance thresholds
    if CONF_MAINTENANCE_OIL_HRS in config:
        cg.add(var.set_maintenance_oil_hrs(config[CONF_MAINTENANCE_OIL_HRS]))
    if CONF_MAINTENANCE_FILTER_HRS in config:
        cg.add(var.set_maintenance_filter_hrs(config[CONF_MAINTENANCE_FILTER_HRS]))
    if CONF_MAINTENANCE_GLOW_HRS in config:
        cg.add(var.set_maintenance_glow_hrs(config[CONF_MAINTENANCE_GLOW_HRS]))
    if CONF_BURN_CYCLE_INTERVAL_HRS in config:
        cg.add(var.set_burn_cycle_interval_hours(config[CONF_BURN_CYCLE_INTERVAL_HRS]))

    # Fuel economy mode
    if CONF_FUEL_ECONOMY in config:
        cg.add(var.enable_fuel_economy(config[CONF_FUEL_ECONOMY]))
    if CONF_FUEL_ECONOMY_REACTIVE in config:
        cg.add(var.enable_fuel_economy_reactive(config[CONF_FUEL_ECONOMY_REACTIVE]))

    # Intelligent prediction
    if CONF_PREDICTION in config:
        cg.add(var.enable_prediction(config[CONF_PREDICTION]))

    # Light sleep
    if CONF_LIGHT_SLEEP in config:
        cg.add(var.enable_light_sleep(config[CONF_LIGHT_SLEEP]))

    # Extended protocol: diagnostic sensors
    for key, setter in [
        (CONF_GLOW_PLUG_CURRENT, "set_glow_plug_current_sensor"),
        (CONF_CHAMBER_TEMP, "set_chamber_temp_sensor"),
        (CONF_BOARD_TEMP, "set_board_temp_sensor"),
        (CONF_ERROR_CODE, "set_error_code_sensor"),
        (CONF_TOTAL_STARTS, "set_total_starts_sensor"),
        ("co_level", "set_co_sensor"),
        ("gps_altitude", "set_altitude_sensor"),
        ("exhaust_temp_direct", "set_exhaust_temp_direct_sensor"),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    # Extended protocol: text sensors
    for key, setter in [
        (CONF_ERROR_TEXT, "set_error_text_sensor"),
        (CONF_FIRMWARE_VERSION, "set_firmware_version_sensor"),
        (CONF_ERROR_LOG, "set_error_log_sensor"),
    ]:
        if key in config:
            txt = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(txt))

    # Extended protocol: unlock button
    if CONF_UNLOCK_BUTTON in config:
        btn_conf = config[CONF_UNLOCK_BUTTON]
        btn = await button.new_button(btn_conf)
        cg.add(btn.set_parent(var))
        await cg.register_component(btn, config)

    # Extended protocol: prime pump button
    if CONF_PRIME_PUMP_BUTTON in config:
        btn_conf = config[CONF_PRIME_PUMP_BUTTON]
        btn = await button.new_button(btn_conf)
        cg.add(btn.set_parent(var))
        cg.add(btn.set_frequency(btn_conf[CONF_PRIME_PUMP_FREQUENCY]))
        await cg.register_component(btn, config)

    # Extended protocol: status report button
    if CONF_STATUS_REPORT_BUTTON in config:
        btn_conf = config[CONF_STATUS_REPORT_BUTTON]
        btn = await button.new_button(btn_conf)
        cg.add(btn.set_parent(var))
        await cg.register_component(btn, config)

    # Extended protocol: diagnostic mode
    if CONF_DIAGNOSTIC_MODE in config:
        cg.add(var.enable_diagnostic_mode(config[CONF_DIAGNOSTIC_MODE]))

    # Eco-Adaptive mode
    if CONF_ECO_ADAPTIVE in config:
        cg.add(var.enable_eco_adaptive(config[CONF_ECO_ADAPTIVE]))
    if CONF_ECO_KP in config or CONF_ECO_KI in config or CONF_ECO_KD in config:
        kp = config.get(CONF_ECO_KP, 1.5)
        ki = config.get(CONF_ECO_KI, 0.3)
        kd = config.get(CONF_ECO_KD, 0.2)
        cg.add(var.set_eco_gains(kp, ki, kd))
    if CONF_ECO_MIN_LEVEL in config or CONF_ECO_MAX_LEVEL in config:
        min_l = config.get(CONF_ECO_MIN_LEVEL, 1)
        max_l = config.get(CONF_ECO_MAX_LEVEL, 9)
        cg.add(var.set_eco_levels(min_l, max_l))
    if CONF_ECO_DEADBAND in config:
        cg.add(var.set_eco_deadband(config[CONF_ECO_DEADBAND]))
    if CONF_ECO_OVERSHOOT_PREDICT in config:
        cg.add(var.enable_eco_overshoot_predict(config[CONF_ECO_OVERSHOOT_PREDICT]))
    if CONF_MAX_PUMP_FREQ in config:
        cg.add(var.set_max_pump_freq(config[CONF_MAX_PUMP_FREQ]))

    # Eco-Adaptive sensors
    for key, setter in [
        (CONF_ECO_ADAPTIVE_LEVEL, "set_eco_adaptive_level_sensor"),
        (CONF_ECO_ADAPTIVE_ERROR, "set_eco_adaptive_error_sensor"),
        (CONF_ECO_POWER_EFFICIENCY, "set_eco_power_efficiency_sensor"),
    ]:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    for key, setter in [
        (CONF_ECO_MODE_STATUS, "set_eco_mode_status_sensor"),
    ]:
        if key in config:
            txt = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(txt))
