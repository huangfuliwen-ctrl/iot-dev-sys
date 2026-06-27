#pragma once

#include <cstdint>

namespace dev_sys {

// ============================================================
// Generic return status codes
// ============================================================
enum class StatusCode : int32_t {
    OK          = 0,
    ERROR       = -1,

    // Device management (100-199)
    DEV_NOT_ACTIVATED   = -100,
    DEV_ALREADY_ACTIVE  = -101,
    DEV_CERT_INVALID    = -102,
    DEV_AUTH_FAILED     = -103,

    // Communication (200-299)
    COMM_TIMEOUT        = -200,
    COMM_DISCONNECTED   = -201,
    COMM_TLS_FAILED     = -202,
    COMM_MSG_TOO_LARGE  = -203,

    // Order (300-399)
    ORDER_NOT_FOUND     = -300,
    ORDER_EXPIRED       = -301,
    ORDER_QUEUE_FULL    = -302,
    ORDER_INVALID_STATE = -303,

    // Recipe (400-499)
    RECIPE_NOT_FOUND    = -400,
    RECIPE_INACTIVE     = -401,
    RECIPE_INCOMPATIBLE = -402,

    // OTA (500-599)
    OTA_DOWNLOAD_FAILED = -500,
    OTA_CHECKSUM_ERROR  = -501,
    OTA_SIGNATURE_ERROR = -502,
    OTA_INSTALL_FAILED  = -503,
    OTA_ROLLBACK_OK     = -504,

    // Fault (600-699)
    FAULT_DETECTED      = -600,
    FAULT_LOCKDOWN      = -601,

    // Storage (700-799)
    STORAGE_FULL        = -700,
    STORAGE_READ_ERROR  = -701,
    STORAGE_WRITE_ERROR = -702,

    // Organization & Account (900-999)
    ORG_NOT_FOUND       = -900,
    ORG_HAS_CHILDREN    = -901,
    ORG_HAS_DEVICES     = -902,
    ORG_HAS_ACCOUNTS    = -903,
    ORG_DUPLICATE_TENANT = -904,

    ACCOUNT_NOT_FOUND   = -910,
    ACCOUNT_LOCKED      = -911,
    ACCOUNT_BAD_PASSWORD = -912,
    ACCOUNT_DUPLICATE   = -913,

    AUTH_TOKEN_EXPIRED   = -920,
    AUTH_TOKEN_INVALID   = -921,
    AUTH_FORBIDDEN       = -922,

    // Hardware (800-899)
    HAL_SENSOR_ERROR    = -800,
    HAL_ACTUATOR_ERROR  = -801,
    HAL_TIMEOUT         = -802,
};

inline const char* status_message(StatusCode code) {
    switch (code) {
        case StatusCode::OK: return "OK";
        case StatusCode::ERROR: return "General error";
        // Device
        case StatusCode::DEV_NOT_ACTIVATED: return "Device not activated";
        case StatusCode::DEV_ALREADY_ACTIVE: return "Device already active";
        case StatusCode::DEV_CERT_INVALID: return "Device certificate invalid";
        case StatusCode::DEV_AUTH_FAILED: return "Device authentication failed";
        // Communication
        case StatusCode::COMM_TIMEOUT: return "Communication timeout";
        case StatusCode::COMM_DISCONNECTED: return "Communication disconnected";
        case StatusCode::COMM_TLS_FAILED: return "TLS handshake failed";
        case StatusCode::COMM_MSG_TOO_LARGE: return "Message too large";
        // Order
        case StatusCode::ORDER_NOT_FOUND: return "Order not found";
        case StatusCode::ORDER_EXPIRED: return "Order expired";
        case StatusCode::ORDER_QUEUE_FULL: return "Order queue full";
        case StatusCode::ORDER_INVALID_STATE: return "Invalid order state transition";
        // Recipe
        case StatusCode::RECIPE_NOT_FOUND: return "Recipe not found";
        case StatusCode::RECIPE_INACTIVE: return "Recipe inactive";
        case StatusCode::RECIPE_INCOMPATIBLE: return "Recipe incompatible with device type";
        // OTA
        case StatusCode::OTA_DOWNLOAD_FAILED: return "OTA download failed";
        case StatusCode::OTA_CHECKSUM_ERROR: return "OTA checksum error";
        case StatusCode::OTA_SIGNATURE_ERROR: return "OTA signature verification failed";
        case StatusCode::OTA_INSTALL_FAILED: return "OTA install failed";
        case StatusCode::OTA_ROLLBACK_OK: return "OTA rolled back successfully";
        // Fault
        case StatusCode::FAULT_DETECTED: return "Fault detected";
        case StatusCode::FAULT_LOCKDOWN: return "Device in fault lockdown";
        // Storage
        case StatusCode::STORAGE_FULL: return "Storage full";
        case StatusCode::STORAGE_READ_ERROR: return "Storage read error";
        case StatusCode::STORAGE_WRITE_ERROR: return "Storage write error";
        // Organization & Account
        case StatusCode::ORG_NOT_FOUND: return "Organization not found";
        case StatusCode::ORG_HAS_CHILDREN: return "Organization has child departments";
        case StatusCode::ORG_HAS_DEVICES: return "Organization has associated devices";
        case StatusCode::ORG_HAS_ACCOUNTS: return "Organization has associated accounts";
        case StatusCode::ORG_DUPLICATE_TENANT: return "Tenant ID already exists";
        case StatusCode::ACCOUNT_NOT_FOUND: return "Account not found";
        case StatusCode::ACCOUNT_LOCKED: return "Account locked (too many failed attempts)";
        case StatusCode::ACCOUNT_BAD_PASSWORD: return "Invalid password";
        case StatusCode::ACCOUNT_DUPLICATE: return "Username already exists";
        case StatusCode::AUTH_TOKEN_EXPIRED: return "Token expired";
        case StatusCode::AUTH_TOKEN_INVALID: return "Token invalid";
        case StatusCode::AUTH_FORBIDDEN: return "Insufficient permissions";
        // Hardware
        case StatusCode::HAL_SENSOR_ERROR: return "Sensor error";
        case StatusCode::HAL_ACTUATOR_ERROR: return "Actuator error";
        case StatusCode::HAL_TIMEOUT: return "Hardware timeout";
        default: return "Unknown error";
    }
}

} // namespace dev_sys
