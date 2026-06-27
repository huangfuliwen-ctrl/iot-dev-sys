#include "database.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>

// Conditional include for SQLite3
#if defined(USE_SYSTEM_SQLITE)
#include <sqlite3.h>
#endif

namespace dev_sys {

struct Database::Impl {
    std::string db_path;
#if defined(USE_SYSTEM_SQLITE)
    sqlite3* handle = nullptr;
#endif
    bool opened = false;
};

Database::Database()
    : impl_(std::make_unique<Impl>()) {}

Database::~Database() {
    close();
}

// ============================================================
// Open / Close
// ============================================================
StatusCode Database::open(const std::string& db_path) {
    impl_->db_path = db_path;
#if defined(USE_SYSTEM_SQLITE)
    int rc = sqlite3_open(db_path.c_str(), &impl_->handle);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] Failed to open: " << sqlite3_errmsg(impl_->handle) << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }
    // WAL mode for better concurrency
    sqlite3_exec(impl_->handle, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->handle, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->handle, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
#endif
    impl_->opened = true;

    StatusCode sc = init_schema();
    if (sc != StatusCode::OK) return sc;

    return migrate_if_needed();
}

StatusCode Database::close() {
#if defined(USE_SYSTEM_SQLITE)
    if (impl_->opened && impl_->handle) {
        sqlite3_close(impl_->handle);
        impl_->handle = nullptr;
    }
#endif
    impl_->opened = false;
    return StatusCode::OK;
}

bool Database::is_open() const {
    return impl_->opened;
}

// ============================================================
// Schema initialization
// ============================================================
StatusCode Database::init_schema() {
    StatusCode sc;

    sc = create_tenants_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_device_types_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_device_models_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_devices_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_activation_tokens_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_audit_log_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_orders_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_recipes_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_faults_table();
    if (sc != StatusCode::OK) return sc;

    sc = create_firmwares_table();
    if (sc != StatusCode::OK) return sc;

    return StatusCode::OK;
}

StatusCode Database::create_tenants_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS tenants (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            tenant_id       TEXT NOT NULL UNIQUE,
            name            TEXT NOT NULL,
            active          INTEGER NOT NULL DEFAULT 1,
            created_at      INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            updated_at      INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
        INSERT OR IGNORE INTO tenants (tenant_id, name) VALUES ('default', 'Default Tenant');
    )";
    return execute(sql);
}

StatusCode Database::create_devices_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS devices (
            id                  INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id           TEXT NOT NULL UNIQUE,
            tenant_id           TEXT NOT NULL DEFAULT 'default',
            product_id          TEXT NOT NULL,
            device_type         INTEGER NOT NULL DEFAULT 4,
            status              INTEGER NOT NULL DEFAULT 5,
            firmware_version    TEXT NOT NULL DEFAULT '0.0.0',
            mac_address         TEXT NOT NULL DEFAULT '',
            model               TEXT NOT NULL DEFAULT '',
            hardware_uid        TEXT NOT NULL UNIQUE,
            last_heartbeat_at   INTEGER NOT NULL DEFAULT 0,
            activated           INTEGER NOT NULL DEFAULT 0,
            activated_at        INTEGER NOT NULL DEFAULT 0,
            created_at          INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            updated_at          INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            FOREIGN KEY (tenant_id) REFERENCES tenants(tenant_id)
        );
        CREATE INDEX IF NOT EXISTS idx_devices_tenant ON devices(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_devices_hwuid ON devices(hardware_uid);
        CREATE INDEX IF NOT EXISTS idx_devices_status ON devices(status);
    )";
    return execute(sql);
}

StatusCode Database::create_activation_tokens_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS activation_tokens (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id   TEXT NOT NULL UNIQUE,
            token       TEXT NOT NULL,
            issued_at   INTEGER NOT NULL,
            expires_at  INTEGER NOT NULL,
            revoked     INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (device_id) REFERENCES devices(device_id)
        );
        CREATE INDEX IF NOT EXISTS idx_act_tokens_device ON activation_tokens(device_id);
    )";
    return execute(sql);
}

StatusCode Database::create_audit_log_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS audit_log (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type  TEXT NOT NULL,
            device_id   TEXT,
            detail      TEXT,
            remote_ip   TEXT,
            success     INTEGER NOT NULL DEFAULT 1,
            created_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
        );
        CREATE INDEX IF NOT EXISTS idx_audit_device ON audit_log(device_id);
        CREATE INDEX IF NOT EXISTS idx_audit_type ON audit_log(event_type);
    )";
    return execute(sql);
}

StatusCode Database::create_device_types_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS device_types (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            type_code       TEXT NOT NULL UNIQUE,
            display_name    TEXT NOT NULL,
            description     TEXT NOT NULL DEFAULT '',
            internal_type   INTEGER NOT NULL DEFAULT 4,
            icon_url        TEXT NOT NULL DEFAULT '',
            sort_order      INTEGER NOT NULL DEFAULT 0,
            is_active       INTEGER NOT NULL DEFAULT 1,
            created_at      TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at      TEXT NOT NULL DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_dt_code ON device_types(type_code);
        -- Seed default types (matching DeviceType enum)
        INSERT OR IGNORE INTO device_types (type_code, display_name, description, internal_type, sort_order)
        VALUES
            ('coffee_machine',  '现磨咖啡机', '支持豆仓研磨、冲泡、奶泡', 1, 1),
            ('instant_machine', '速溶饮料机', '支持多种粉料混合冲泡', 2, 2),
            ('water_dispenser', '饮水机',     '支持常温/热水/冰水出水', 3, 3),
            ('other',           '其他终端',   '其他自动售卖/服务终端', 4, 99);
    )";
    return execute(sql);
}

StatusCode Database::create_device_models_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS device_models (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            model_code      TEXT NOT NULL UNIQUE,
            type_code       TEXT NOT NULL,
            display_name    TEXT NOT NULL,
            description     TEXT NOT NULL DEFAULT '',
            specs_json      TEXT NOT NULL DEFAULT '{}',
            firmware_base   TEXT NOT NULL DEFAULT '1.0.0',
            is_active       INTEGER NOT NULL DEFAULT 1,
            sort_order      INTEGER NOT NULL DEFAULT 0,
            created_at      TEXT NOT NULL DEFAULT (datetime('now')),
            updated_at      TEXT NOT NULL DEFAULT (datetime('now')),
            FOREIGN KEY (type_code) REFERENCES device_types(type_code)
        );
        CREATE INDEX IF NOT EXISTS idx_dm_code ON device_models(model_code);
        CREATE INDEX IF NOT EXISTS idx_dm_type ON device_models(type_code);
    )";
    return execute(sql);
}

StatusCode Database::migrate_if_needed() {
    // TODO: version-based schema migration
    return StatusCode::OK;
}

// ============================================================
// Generic SQL
// ============================================================
StatusCode Database::execute(const std::string& sql) {
    if (!impl_->opened) return StatusCode::STORAGE_WRITE_ERROR;
#if defined(USE_SYSTEM_SQLITE)
    char* err_msg = nullptr;
    int rc = sqlite3_exec(impl_->handle, sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] Execute error: " << (err_msg ? err_msg : "unknown") << std::endl;
        if (err_msg) sqlite3_free(err_msg);
        return StatusCode::STORAGE_WRITE_ERROR;
    }
#endif
    return StatusCode::OK;
}

StatusCode Database::query(const std::string& sql,
                            std::vector<std::vector<std::string>>& rows) {
    if (!impl_->opened) return StatusCode::STORAGE_READ_ERROR;
#if defined(USE_SYSTEM_SQLITE)
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(impl_->handle, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "[DB] Query prepare error: " << sqlite3_errmsg(impl_->handle) << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }

    int col_count = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int i = 0; i < col_count; i++) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.push_back(text ? std::string(text) : "");
        }
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[DB] Query step error: " << sqlite3_errmsg(impl_->handle) << std::endl;
        return StatusCode::STORAGE_READ_ERROR;
    }
#endif
    return StatusCode::OK;
}

// ============================================================
// Device CRUD
// ============================================================
Device Database::device_from_row(const std::vector<std::string>& row) const {
    Device dev;
    // Column order: id, device_id, tenant_id, product_id, device_type,
    //               status, firmware_version, mac_address, model, hardware_uid,
    //               last_heartbeat_at, activated, activated_at, created_at, updated_at
    if (row.size() < 11) return dev;
    // row[0] = id (skip)
    dev.device_id         = row[1];
    dev.tenant_id         = row[2];
    dev.product_id        = row[3];
    dev.type              = static_cast<DeviceType>(std::stoi(row[4]));
    dev.status            = static_cast<DeviceStatus>(std::stoi(row[5]));
    dev.firmware_version  = row[6];
    dev.mac_address       = row[7];
    dev.model             = row[8];
    dev.hardware_uid      = row[9];
    dev.last_heartbeat_at = row.size() > 10 ? std::stoll(row[10]) : 0;
    dev.activated         = row.size() > 11 ? (std::stoi(row[11]) != 0) : false;
    dev.activated_at      = row.size() > 12 ? std::stoll(row[12]) : 0;
    return dev;
}

StatusCode Database::insert_device(const Device& device) {
    std::ostringstream sql;
    sql << "INSERT INTO devices "
        << "(device_id, tenant_id, product_id, device_type, status, "
        << "firmware_version, mac_address, model, hardware_uid, "
        << "last_heartbeat_at, activated, activated_at) VALUES ("
        << "'" << device.device_id << "',"
        << "'" << device.tenant_id << "',"
        << "'" << device.product_id << "',"
        << static_cast<int>(device.type) << ","
        << static_cast<int>(device.status) << ","
        << "'" << device.firmware_version << "',"
        << "'" << device.mac_address << "',"
        << "'" << device.model << "',"
        << "'" << device.hardware_uid << "',"
        << device.last_heartbeat_at << ","
        << (device.activated ? 1 : 0) << ","
        << device.activated_at
        << ")";
    return execute(sql.str());
}

StatusCode Database::update_device(const Device& device) {
    std::ostringstream sql;
    sql << "UPDATE devices SET "
        << "tenant_id='" << device.tenant_id << "',"
        << "status=" << static_cast<int>(device.status) << ","
        << "firmware_version='" << device.firmware_version << "',"
        << "last_heartbeat_at=" << device.last_heartbeat_at << ","
        << "activated=" << (device.activated ? 1 : 0) << ","
        << "activated_at=" << device.activated_at << ","
        << "updated_at=strftime('%s','now') "
        << "WHERE device_id='" << device.device_id << "'";
    return execute(sql.str());
}

StatusCode Database::upsert_device(const Device& device) {
    std::ostringstream sql;
    sql << "INSERT INTO devices "
        << "(device_id, tenant_id, product_id, device_type, status, "
        << "firmware_version, mac_address, model, hardware_uid, "
        << "last_heartbeat_at, activated, activated_at) VALUES ("
        << "'" << device.device_id << "',"
        << "'" << device.tenant_id << "',"
        << "'" << device.product_id << "',"
        << static_cast<int>(device.type) << ","
        << static_cast<int>(device.status) << ","
        << "'" << device.firmware_version << "',"
        << "'" << device.mac_address << "',"
        << "'" << device.model << "',"
        << "'" << device.hardware_uid << "',"
        << device.last_heartbeat_at << ","
        << (device.activated ? 1 : 0) << ","
        << device.activated_at
        << ") ON CONFLICT(device_id) DO UPDATE SET "
        << "status=excluded.status,"
        << "firmware_version=excluded.firmware_version,"
        << "last_heartbeat_at=excluded.last_heartbeat_at,"
        << "updated_at=strftime('%s','now')";
    return execute(sql.str());
}

std::optional<Device> Database::get_device(const std::string& device_id) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM devices WHERE device_id='" + device_id + "'";
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;
    return device_from_row(rows[0]);
}

std::optional<Device> Database::get_device_by_hwuid(const std::string& hardware_uid) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM devices WHERE hardware_uid='" + hardware_uid + "'";
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;
    return device_from_row(rows[0]);
}

std::vector<Device> Database::list_devices_by_tenant(const std::string& tenant_id) {
    std::vector<Device> result;
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM devices WHERE tenant_id='" + tenant_id + "'";
    StatusCode sc = query(sql, rows);
    if (sc == StatusCode::OK) {
        for (const auto& row : rows) {
            result.push_back(device_from_row(row));
        }
    }
    return result;
}

std::vector<Device> Database::list_all_devices() {
    std::vector<Device> result;
    std::vector<std::vector<std::string>> rows;
    StatusCode sc = query("SELECT * FROM devices", rows);
    if (sc == StatusCode::OK) {
        for (const auto& row : rows) {
            result.push_back(device_from_row(row));
        }
    }
    return result;
}

bool Database::device_exists(const std::string& hardware_uid) const {
    std::vector<std::vector<std::string>> rows;
    // Need to cast away const for query, but it's logically const
    std::string sql = "SELECT COUNT(*) FROM devices WHERE hardware_uid='" + hardware_uid + "'";
    const_cast<Database*>(this)->query(sql, rows);
    return !rows.empty() && !rows[0].empty() && rows[0][0] != "0";
}

// ============================================================
// Activation Token
// ============================================================
StatusCode Database::store_activation_token(const std::string& device_id,
                                              const std::string& token,
                                              int64_t issued_at,
                                              int64_t expires_at) {
    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO activation_tokens "
        << "(device_id, token, issued_at, expires_at, revoked) VALUES ("
        << "'" << device_id << "',"
        << "'" << token << "',"
        << issued_at << ","
        << expires_at << ","
        << "0)";
    return execute(sql.str());
}

std::optional<std::string> Database::get_activation_token(const std::string& device_id) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT token, expires_at, revoked FROM activation_tokens WHERE device_id='"
                      + device_id + "'";
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;

    // Check if revoked or expired
    if (rows[0].size() >= 3) {
        bool revoked = (std::stoi(rows[0][2]) != 0);
        int64_t expires = std::stoll(rows[0][1]);
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (revoked || now > expires) return std::nullopt;
    }
    return rows[0][0];
}

// ============================================================
// Audit Log
// ============================================================
StatusCode Database::log_activation(const std::string& device_id,
                                      const std::string& hardware_uid,
                                      const std::string& remote_ip,
                                      bool success,
                                      const std::string& message) {
    std::ostringstream sql;
    sql << "INSERT INTO audit_log (event_type, device_id, detail, remote_ip, success) VALUES ("
        << "'device_activation',"
        << (device_id.empty() ? "NULL" : ("'" + device_id + "'")) << ","
        << "'hardware_uid=" << hardware_uid << " msg=" << message << "',"
        << "'" << remote_ip << "',"
        << (success ? "1" : "0")
        << ")";
    return execute(sql.str());
}

// ============================================================
// Device Types CRUD
// ============================================================
DeviceTypeInfo Database::type_from_row(const std::vector<std::string>& row) const {
    DeviceTypeInfo info;
    if (row.size() < 8) return info;
    info.id            = std::stoi(row[0]);
    info.type_code     = row[1];
    info.display_name  = row[2];
    info.description   = row[3];
    info.internal_type = std::stoi(row[4]);
    info.icon_url      = row.size() > 5 ? row[5] : "";
    info.sort_order    = row.size() > 6 ? std::stoi(row[6]) : 0;
    info.is_active     = row.size() > 7 ? (std::stoi(row[7]) != 0) : true;
    info.created_at    = row.size() > 8 ? row[8] : "";
    info.updated_at    = row.size() > 9 ? row[9] : "";
    return info;
}

StatusCode Database::insert_device_type(const DeviceTypeInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO device_types "
        << "(type_code, display_name, description, internal_type, icon_url, sort_order) VALUES ("
        << "'" << info.type_code << "','" << info.display_name << "','" << info.description << "',"
        << info.internal_type << ",'" << info.icon_url << "'," << info.sort_order << ")";
    return execute(sql.str());
}

StatusCode Database::update_device_type(const DeviceTypeInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE device_types SET display_name='" << info.display_name
        << "',description='" << info.description
        << "',internal_type=" << info.internal_type
        << ",icon_url='" << info.icon_url
        << "',sort_order=" << info.sort_order
        << ",is_active=" << (info.is_active ? 1 : 0)
        << ",updated_at=datetime('now') WHERE type_code='" << info.type_code << "'";
    return execute(sql.str());
}

StatusCode Database::delete_device_type(const std::string& type_code) {
    std::string sql = "UPDATE device_types SET is_active=0, updated_at=datetime('now') "
                      "WHERE type_code='" + type_code + "'";
    return execute(sql);
}

std::optional<DeviceTypeInfo> Database::get_device_type(const std::string& type_code) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM device_types WHERE type_code='" + type_code + "'", rows);
    if (rows.empty()) return std::nullopt;
    return type_from_row(rows[0]);
}

std::vector<DeviceTypeInfo> Database::list_device_types(bool active_only) {
    std::vector<DeviceTypeInfo> result;
    std::string sql = "SELECT * FROM device_types";
    if (active_only) sql += " WHERE is_active=1";
    sql += " ORDER BY sort_order ASC";
    std::vector<std::vector<std::string>> rows;
    if (query(sql, rows) == StatusCode::OK)
        for (const auto& r : rows) result.push_back(type_from_row(r));
    return result;
}

// ============================================================
// Device Models CRUD
// ============================================================
DeviceModelInfo Database::model_from_row(const std::vector<std::string>& row) const {
    DeviceModelInfo info;
    if (row.size() < 7) return info;
    info.id            = std::stoi(row[0]);
    info.model_code    = row[1];
    info.type_code     = row[2];
    info.display_name  = row[3];
    info.description   = row.size() > 4 ? row[4] : "";
    info.specs_json    = row.size() > 5 ? row[5] : "{}";
    info.firmware_base = row.size() > 6 ? row[6] : "1.0.0";
    info.is_active     = row.size() > 7 ? (std::stoi(row[7]) != 0) : true;
    info.sort_order    = row.size() > 8 ? std::stoi(row[8]) : 0;
    info.created_at    = row.size() > 9 ? row[9] : "";
    info.updated_at    = row.size() > 10 ? row[10] : "";
    return info;
}

StatusCode Database::insert_device_model(const DeviceModelInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO device_models "
        << "(model_code, type_code, display_name, description, specs_json, firmware_base, sort_order) VALUES ("
        << "'" << info.model_code << "','" << info.type_code << "','" << info.display_name << "',"
        << "'" << info.description << "','" << info.specs_json << "','" << info.firmware_base << "',"
        << info.sort_order << ")";
    return execute(sql.str());
}

StatusCode Database::update_device_model(const DeviceModelInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE device_models SET type_code='" << info.type_code
        << "',display_name='" << info.display_name
        << "',description='" << info.description
        << "',specs_json='" << info.specs_json
        << "',firmware_base='" << info.firmware_base
        << "',is_active=" << (info.is_active ? 1 : 0)
        << ",sort_order=" << info.sort_order
        << ",updated_at=datetime('now') WHERE model_code='" << info.model_code << "'";
    return execute(sql.str());
}

StatusCode Database::delete_device_model(const std::string& model_code) {
    return execute("UPDATE device_models SET is_active=0, updated_at=datetime('now') "
                   "WHERE model_code='" + model_code + "'");
}

std::optional<DeviceModelInfo> Database::get_device_model(const std::string& model_code) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM device_models WHERE model_code='" + model_code + "'", rows);
    if (rows.empty()) return std::nullopt;
    return model_from_row(rows[0]);
}

std::vector<DeviceModelInfo> Database::list_device_models(const std::string& type_code, bool active_only) {
    std::vector<DeviceModelInfo> result;
    std::string sql = "SELECT * FROM device_models WHERE 1=1";
    if (!type_code.empty()) sql += " AND type_code='" + type_code + "'";
    if (active_only) sql += " AND is_active=1";
    sql += " ORDER BY type_code, sort_order ASC";
    std::vector<std::vector<std::string>> rows;
    if (query(sql, rows) == StatusCode::OK)
        for (const auto& r : rows) result.push_back(model_from_row(r));
    return result;
}


// ============================================================
// Orders table
// ============================================================
StatusCode Database::create_orders_table() {
    return execute(R"(
        CREATE TABLE IF NOT EXISTS orders (
            id INTEGER PRIMARY KEY AUTOINCREMENT, order_id TEXT NOT NULL UNIQUE,
            tenant_id TEXT NOT NULL DEFAULT 'default', device_id TEXT NOT NULL,
            recipe_id TEXT NOT NULL DEFAULT '', cup_size TEXT NOT NULL DEFAULT '',
            quantity INTEGER NOT NULL DEFAULT 1, total_amount INTEGER NOT NULL DEFAULT 0,
            payment_method TEXT NOT NULL DEFAULT '', status INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL DEFAULT '', failure_reason TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(tenant_id)
        );
        CREATE INDEX IF NOT EXISTS idx_orders_tenant ON orders(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_orders_device ON orders(device_id);
    )");
}

StatusCode Database::insert_order(const Order& order) {
    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO orders (order_id,tenant_id,device_id,recipe_id,cup_size,quantity,total_amount,payment_method,status,created_at,failure_reason) VALUES ('"
        << order.order_id << "','" << order.tenant_id << "','" << order.device_id << "','"
        << order.recipe_id << "','" << order.cup_size << "'," << order.quantity << ","
        << order.total_amount << ",'" << order.payment_method << "',"
        << static_cast<int>(order.status) << ",'" << order.created_at << "','"
        << order.failure_reason << "')";
    return execute(sql.str());
}

StatusCode Database::update_order_status(const std::string& order_id, int status) {
    return execute("UPDATE orders SET status=" + std::to_string(status) + " WHERE order_id='" + order_id + "'");
}

std::optional<Order> Database::get_order(const std::string& order_id) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM orders WHERE order_id='" + order_id + "'", rows);
    if (!rows.empty()) return order_from_row(rows[0]);
    return std::nullopt;
}

std::vector<Order> Database::list_all_orders() {
    std::vector<Order> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM orders ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(order_from_row(row));
    return result;
}

Order Database::order_from_row(const std::vector<std::string>& row) const {
    Order o;
    if (row.size() < 11) return o;
    o.order_id=row[1]; o.tenant_id=row[2]; o.device_id=row[3]; o.recipe_id=row[4];
    o.cup_size=row[5]; o.quantity=std::stoi(row[6]); o.total_amount=std::stoi(row[7]);
    o.payment_method=row[8]; o.status=static_cast<OrderStatus>(std::stoi(row[9]));
    o.created_at=row[10]; o.failure_reason=row.size()>11?row[11]:"";
    return o;
}

// ============================================================
// Recipes table
// ============================================================
StatusCode Database::create_recipes_table() {
    return execute(R"(
        CREATE TABLE IF NOT EXISTS recipes (
            id INTEGER PRIMARY KEY AUTOINCREMENT, recipe_id TEXT NOT NULL UNIQUE,
            recipe_name TEXT NOT NULL, device_type INTEGER NOT NULL DEFAULT 1,
            category TEXT NOT NULL DEFAULT '', is_active INTEGER NOT NULL DEFAULT 1,
            version INTEGER NOT NULL DEFAULT 1, description TEXT NOT NULL DEFAULT ''
        );
    )");
}

StatusCode Database::insert_recipe(const Recipe& recipe) {
    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO recipes (recipe_id,recipe_name,device_type,category,is_active,version,description) VALUES ('"
        << recipe.recipe_id << "','" << recipe.recipe_name << "',"
        << static_cast<int>(recipe.device_type) << ",'" << recipe.category << "',"
        << (recipe.is_active?1:0) << "," << recipe.version << ",'" << recipe.description << "')";
    return execute(sql.str());
}

StatusCode Database::update_recipe(const Recipe& recipe) { return insert_recipe(recipe); }
StatusCode Database::delete_recipe(const std::string& recipe_id) { return execute("UPDATE recipes SET is_active=0 WHERE recipe_id='"+recipe_id+"'"); }

std::optional<Recipe> Database::get_recipe(const std::string& recipe_id) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM recipes WHERE recipe_id='"+recipe_id+"'", rows);
    if (!rows.empty()) return recipe_from_row(rows[0]);
    return std::nullopt;
}

std::vector<Recipe> Database::list_all_recipes() {
    std::vector<Recipe> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM recipes WHERE is_active=1", rows);
    for (const auto& row : rows) result.push_back(recipe_from_row(row));
    return result;
}

Recipe Database::recipe_from_row(const std::vector<std::string>& row) const {
    Recipe r;
    if (row.size()<8) return r;
    r.recipe_id=row[1]; r.recipe_name=row[2]; r.device_type=static_cast<DeviceType>(std::stoi(row[3]));
    r.category=row[4]; r.is_active=(std::stoi(row[5])!=0); r.version=std::stoi(row[6]); r.description=row[7];
    return r;
}

// ============================================================
// Faults table
// ============================================================
StatusCode Database::create_faults_table() {
    return execute(R"(
        CREATE TABLE IF NOT EXISTS faults (
            id INTEGER PRIMARY KEY AUTOINCREMENT, tenant_id TEXT NOT NULL DEFAULT 'default',
            device_id TEXT NOT NULL, code INTEGER NOT NULL, level INTEGER NOT NULL DEFAULT 1,
            description TEXT NOT NULL DEFAULT '', timestamp TEXT NOT NULL DEFAULT '',
            resolved INTEGER NOT NULL DEFAULT 0, sensor_snapshot TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_faults_device ON faults(tenant_id, device_id);
    )");
}

StatusCode Database::insert_fault(const FaultInfo& fault) {
    std::ostringstream sql;
    sql << "INSERT INTO faults (tenant_id,device_id,code,level,description,timestamp,sensor_snapshot) VALUES ('"
        << fault.tenant_id << "','" << fault.device_id << "',"
        << static_cast<int>(fault.code) << "," << static_cast<int>(fault.level) << ",'"
        << fault.description << "','" << fault.timestamp << "','" << fault.sensor_snapshot << "')";
    return execute(sql.str());
}

StatusCode Database::resolve_fault(const std::string& tenant_id, const std::string& device_id, int code) {
    return execute("UPDATE faults SET resolved=1 WHERE tenant_id='"+tenant_id+"' AND device_id='"+device_id+"' AND code="+std::to_string(code));
}

std::vector<FaultInfo> Database::list_all_faults() {
    std::vector<FaultInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM faults ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(fault_from_row(row));
    return result;
}

std::vector<FaultInfo> Database::list_active_faults() {
    std::vector<FaultInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM faults WHERE resolved=0 ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(fault_from_row(row));
    return result;
}

std::vector<FaultInfo> Database::list_faults_by_device(const std::string& tenant_id, const std::string& device_id) {
    std::vector<FaultInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM faults WHERE tenant_id='"+tenant_id+"' AND device_id='"+device_id+"' ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(fault_from_row(row));
    return result;
}

FaultInfo Database::fault_from_row(const std::vector<std::string>& row) const {
    FaultInfo f;
    if (row.size()<7) return f;
    f.tenant_id=row[1]; f.device_id=row[2]; f.code=static_cast<FaultCode>(std::stoi(row[3]));
    f.level=static_cast<FaultLevel>(std::stoi(row[4])); f.description=row[5]; f.timestamp=row[6];
    f.sensor_snapshot=row.size()>8?row[8]:"";
    return f;
}

// ============================================================
// Firmwares table
// ============================================================
StatusCode Database::create_firmwares_table() {
    return execute(R"(
        CREATE TABLE IF NOT EXISTS firmwares (
            id INTEGER PRIMARY KEY AUTOINCREMENT, version TEXT NOT NULL UNIQUE,
            product_id TEXT NOT NULL DEFAULT '', download_url TEXT NOT NULL DEFAULT '',
            checksum_sha256 TEXT NOT NULL DEFAULT '', changelog TEXT NOT NULL DEFAULT '',
            force_upgrade INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0
        );
    )");
}

StatusCode Database::insert_firmware(const FirmwareVersion& fw) {
    std::ostringstream sql;
    sql << "INSERT OR REPLACE INTO firmwares (version,product_id,download_url,checksum_sha256,changelog,force_upgrade,created_at) VALUES ('"
        << fw.version << "','" << fw.product_id << "','" << fw.download_url << "','"
        << fw.checksum_sha256 << "','" << fw.changelog << "',"
        << (fw.force_upgrade?1:0) << "," << fw.created_at << ")";
    return execute(sql.str());
}

StatusCode Database::delete_firmware(const std::string& version) { return execute("DELETE FROM firmwares WHERE version='"+version+"'"); }

std::optional<FirmwareVersion> Database::get_firmware(const std::string& version) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM firmwares WHERE version='"+version+"'", rows);
    if (!rows.empty()) return firmware_from_row(rows[0]);
    return std::nullopt;
}

std::vector<FirmwareVersion> Database::list_all_firmwares() {
    std::vector<FirmwareVersion> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM firmwares ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(firmware_from_row(row));
    return result;
}

FirmwareVersion Database::firmware_from_row(const std::vector<std::string>& row) const {
    FirmwareVersion fw;
    if (row.size()<7) return fw;
    fw.version=row[1]; fw.product_id=row[2]; fw.download_url=row[3];
    fw.checksum_sha256=row[4]; fw.changelog=row[5];
    fw.force_upgrade=(std::stoi(row[6])!=0); fw.created_at=std::stoll(row[7]);
    return fw;
}


// ============================================================
// Maintenance
// ============================================================
StatusCode Database::vacuum() {
    return execute("VACUUM");
}

StatusCode Database::clean_expired_data(int retention_days) {
    std::ostringstream sql;
    sql << "DELETE FROM audit_log WHERE created_at < strftime('%s','now') - "
        << (retention_days * 86400);
    execute(sql.str());

    // Revoke expired tokens
    sql.str("");
    sql << "UPDATE activation_tokens SET revoked=1 WHERE expires_at < strftime('%s','now')";
    execute(sql.str());

    return StatusCode::OK;
}

StatusCode Database::backup(const std::string& backup_path) {
    // TODO: sqlite3_backup API
    return StatusCode::OK;
}

} // namespace dev_sys
