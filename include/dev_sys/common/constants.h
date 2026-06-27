#pragma once

namespace dev_sys {

// ============================================================
// System-wide constants
// ============================================================

// Communication
constexpr int MQTT_DEFAULT_KEEPALIVE_SEC    = 60;
constexpr int MQTT_QOS                       = 1;
constexpr int MQTT_MAX_MESSAGE_SIZE          = 256 * 1024;  // 256KB
constexpr int MQTT_RECONNECT_BASE_MS         = 1000;
constexpr int MQTT_RECONNECT_MAX_MS          = 60000;

// Heartbeat
constexpr int HEARTBEAT_INTERVAL_SEC_DEFAULT = 60;
constexpr int HEARTBEAT_MISS_THRESHOLD       = 3;

// Orders
constexpr int ORDER_EXPIRE_MINUTES_DEFAULT   = 15;
constexpr int ORDER_MAX_QUEUE_DEPTH          = 10;

// OTA
constexpr int OTA_CHECK_INTERVAL_HOURS       = 24;
constexpr int OTA_CHECK_HOUR_OF_DAY          = 3;  // 3:00 AM
constexpr int OTA_INSTALL_TIMEOUT_MINUTES    = 5;
constexpr int OTA_BOOT_FAIL_THRESHOLD        = 3;

// Fault detection
constexpr int FAULT_HIGH_PRIORITY_REPORT_SEC = 3;
constexpr int FAULT_LOW_PRIORITY_REPORT_SEC  = 300;

// Storage
constexpr int LOG_MAX_FILE_SIZE_MB           = 50;
constexpr int LOG_MAX_FILES                  = 10;

// Hardware
constexpr int SENSOR_SAMPLE_INTERVAL_MS      = 100;
constexpr int ACTUATOR_MAX_RUN_TIME_MS       = 300000;
constexpr int EMERGENCY_STOP_TIMEOUT_MS      = 200;
constexpr int HEATER_PID_COMPUTE_INTERVAL_MS = 1000;

} // namespace dev_sys
