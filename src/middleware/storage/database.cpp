#include "database.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef HAS_LIBPQ
#include <libpq-fe.h>
#endif

namespace dev_sys {

// ============================================================
// Utility: escape string literal for SQL
// ============================================================
namespace {
    std::string sql_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\'') out += "''";
            else out += c;
        }
        return out;
    }

    std::string sql_str(const std::string& s) {
        return "'" + sql_escape(s) + "'";
    }

    std::string sql_nullable_str(const std::string& s) {
        return s.empty() ? "NULL" : sql_str(s);
    }

    std::string sql_bool(bool v) { return v ? "TRUE" : "FALSE"; }
}

struct Database::Impl {
    std::string conn_string;
    bool opened = false;
    mutable std::mutex mutex; // serialize DB access

#ifdef HAS_LIBPQ
    PGconn* conn = nullptr;

    // Check PGresult status, log error, and clear
    bool check_result(PGresult* res, const char* context = nullptr) {
        if (!res) {
            std::cerr << "[DB] PGresult is NULL" << (context ? " @ " : "")
                      << (context ? context : "") << std::endl;
            return false;
        }
        ExecStatusType st = PQresultStatus(res);
        if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
            std::cerr << "[DB] PG error: " << PQresultErrorMessage(res)
                      << (context ? " @ " : "") << (context ? context : "") << std::endl;
            PQclear(res);
            return false;
        }
        PQclear(res);
        return true;
    }

    // Execute a SQL command (no rows returned)
    bool exec_cmd(const char* sql, const char* context = nullptr) {
        PGresult* res = PQexec(conn, sql);
        return check_result(res, context);
    }
#endif
};

Database::Database()
    : impl_(std::make_unique<Impl>()) {}

Database::~Database() {
    close();
}

// ============================================================
// Open / Close
// ============================================================
StatusCode Database::open(const std::string& connection_string) {
    impl_->conn_string = connection_string;

#ifdef HAS_LIBPQ
    impl_->conn = PQconnectdb(connection_string.c_str());
    ConnStatusType st = PQstatus(impl_->conn);
    if (st != CONNECTION_OK) {
        std::cerr << "[DB] PostgreSQL connection failed: "
                  << PQerrorMessage(impl_->conn) << std::endl;
        PQfinish(impl_->conn);
        impl_->conn = nullptr;
        return StatusCode::STORAGE_READ_ERROR;
    }

    std::cout << "[DB] Connected to PostgreSQL. "
              << "Server: " << PQparameterStatus(impl_->conn, "server_version")
              << std::endl;

    impl_->opened = true;

    StatusCode sc = init_schema();
    if (sc != StatusCode::OK) {
        close();
        return sc;
    }

    return migrate_if_needed();
#else
    std::cerr << "[DB] WARNING: Built without libpq. Using in-memory storage (data will NOT persist)." << std::endl;
    impl_->opened = true;
    // Run schema init on in-memory stubs
    init_schema();
    return migrate_if_needed();
#endif
}

StatusCode Database::close() {
#ifdef HAS_LIBPQ
    if (impl_->opened && impl_->conn) {
        PQfinish(impl_->conn);
        impl_->conn = nullptr;
    }
#endif
    impl_->opened = false;
    return StatusCode::OK;
}

bool Database::is_open() const {
    return impl_->opened;
}

// ============================================================
// Schema initialization — PostgreSQL DDL
// ============================================================
StatusCode Database::init_schema() {
    StatusCode sc;
    sc = create_tenants_table();          if (sc != StatusCode::OK) return sc;
    sc = create_device_types_table();     if (sc != StatusCode::OK) return sc;
    sc = create_device_models_table();    if (sc != StatusCode::OK) return sc;
    sc = create_devices_table();          if (sc != StatusCode::OK) return sc;
    sc = create_activation_tokens_table();if (sc != StatusCode::OK) return sc;
    sc = create_audit_log_table();        if (sc != StatusCode::OK) return sc;
    sc = create_orders_table();           if (sc != StatusCode::OK) return sc;
    sc = create_recipes_table();          if (sc != StatusCode::OK) return sc;
    sc = create_faults_table();           if (sc != StatusCode::OK) return sc;
    sc = create_firmwares_table();        if (sc != StatusCode::OK) return sc;
    sc = create_organizations_table();    if (sc != StatusCode::OK) return sc;
    sc = create_accounts_table();         if (sc != StatusCode::OK) return sc;
    sc = create_mqtt_tenant_config_table(); if (sc != StatusCode::OK) return sc;
    return StatusCode::OK;
}

StatusCode Database::create_tenants_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS tenants (
            id              SERIAL PRIMARY KEY,
            tenant_id       TEXT NOT NULL UNIQUE,
            name            TEXT NOT NULL,
            active          BOOLEAN NOT NULL DEFAULT TRUE,
            created_at      BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW())::BIGINT),
            updated_at      BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW())::BIGINT)
        );
        INSERT INTO tenants (tenant_id, name)
        VALUES ('default', 'Default Tenant')
        ON CONFLICT (tenant_id) DO NOTHING;
    )";
    return execute(sql);
}

StatusCode Database::create_devices_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS devices (
            id                  SERIAL PRIMARY KEY,
            device_id           TEXT NOT NULL UNIQUE,
            tenant_id           TEXT NOT NULL DEFAULT 'default',
            product_id          TEXT NOT NULL,
            device_type         INTEGER NOT NULL DEFAULT 4,
            status              INTEGER NOT NULL DEFAULT 5,
            firmware_version    TEXT NOT NULL DEFAULT '0.0.0',
            mac_address         TEXT NOT NULL DEFAULT '',
            model               TEXT NOT NULL DEFAULT '',
            hardware_uid        TEXT NOT NULL UNIQUE,
            last_heartbeat_at   BIGINT NOT NULL DEFAULT 0,
            activated           BOOLEAN NOT NULL DEFAULT FALSE,
            activated_at        BIGINT NOT NULL DEFAULT 0,
            created_at          BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW())::BIGINT),
            updated_at          BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW())::BIGINT),
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
            id          SERIAL PRIMARY KEY,
            device_id   TEXT NOT NULL UNIQUE,
            token       TEXT NOT NULL,
            issued_at   BIGINT NOT NULL,
            expires_at  BIGINT NOT NULL,
            revoked     BOOLEAN NOT NULL DEFAULT FALSE,
            FOREIGN KEY (device_id) REFERENCES devices(device_id)
        );
        CREATE INDEX IF NOT EXISTS idx_act_tokens_device ON activation_tokens(device_id);
    )";
    return execute(sql);
}

StatusCode Database::create_audit_log_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS audit_log (
            id          SERIAL PRIMARY KEY,
            event_type  TEXT NOT NULL,
            device_id   TEXT,
            detail      TEXT,
            remote_ip   TEXT,
            success     BOOLEAN NOT NULL DEFAULT TRUE,
            created_at  BIGINT NOT NULL DEFAULT (EXTRACT(EPOCH FROM NOW())::BIGINT)
        );
        CREATE INDEX IF NOT EXISTS idx_audit_device ON audit_log(device_id);
        CREATE INDEX IF NOT EXISTS idx_audit_type ON audit_log(event_type);
    )";
    return execute(sql);
}

StatusCode Database::create_device_types_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS device_types (
            id              SERIAL PRIMARY KEY,
            type_code       TEXT NOT NULL UNIQUE,
            display_name    TEXT NOT NULL,
            description     TEXT NOT NULL DEFAULT '',
            internal_type   INTEGER NOT NULL DEFAULT 4,
            icon_url        TEXT NOT NULL DEFAULT '',
            sort_order      INTEGER NOT NULL DEFAULT 0,
            is_active       BOOLEAN NOT NULL DEFAULT TRUE,
            created_at      TEXT NOT NULL DEFAULT NOW()::TEXT,
            updated_at      TEXT NOT NULL DEFAULT NOW()::TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_dt_code ON device_types(type_code);

        INSERT INTO device_types (type_code, display_name, description, internal_type, sort_order)
        VALUES
            ('coffee_machine',  '现磨咖啡机', '支持豆仓研磨、冲泡、奶泡', 1, 1),
            ('instant_machine', '速溶饮料机', '支持多种粉料混合冲泡', 2, 2),
            ('water_dispenser', '饮水机',     '支持常温/热水/冰水出水', 3, 3),
            ('other',           '其他终端',   '其他自动售卖/服务终端', 4, 99)
        ON CONFLICT (type_code) DO NOTHING;
    )";
    return execute(sql);
}

StatusCode Database::create_device_models_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS device_models (
            id              SERIAL PRIMARY KEY,
            model_code      TEXT NOT NULL UNIQUE,
            model_key       TEXT NOT NULL DEFAULT '',
            type_code       TEXT NOT NULL,
            display_name    TEXT NOT NULL,
            description     TEXT NOT NULL DEFAULT '',
            specs_json      TEXT NOT NULL DEFAULT '{}',
            firmware_base   TEXT NOT NULL DEFAULT '1.0.0',
            is_active       BOOLEAN NOT NULL DEFAULT TRUE,
            sort_order      INTEGER NOT NULL DEFAULT 0,
            created_at      TEXT NOT NULL DEFAULT NOW()::TEXT,
            updated_at      TEXT NOT NULL DEFAULT NOW()::TEXT,
            FOREIGN KEY (type_code) REFERENCES device_types(type_code)
        );
        CREATE INDEX IF NOT EXISTS idx_dm_code ON device_models(model_code);
        CREATE INDEX IF NOT EXISTS idx_dm_type ON device_models(type_code);
    )";
    return execute(sql);
}

StatusCode Database::create_orders_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS orders (
            id              SERIAL PRIMARY KEY,
            order_id        TEXT NOT NULL UNIQUE,
            tenant_id       TEXT NOT NULL DEFAULT 'default',
            device_id       TEXT NOT NULL,
            recipe_id       TEXT NOT NULL DEFAULT '',
            recipe_name     TEXT NOT NULL DEFAULT '',
            cup_size        TEXT NOT NULL DEFAULT '',
            quantity        INTEGER NOT NULL DEFAULT 1,
            total_amount    INTEGER NOT NULL DEFAULT 0,
            payment_method  TEXT NOT NULL DEFAULT '',
            status          INTEGER NOT NULL DEFAULT 0,
            created_at      BIGINT NOT NULL DEFAULT 0,
            failure_reason  TEXT NOT NULL DEFAULT '',
            FOREIGN KEY (tenant_id) REFERENCES tenants(tenant_id)
        );
        CREATE INDEX IF NOT EXISTS idx_orders_tenant ON orders(tenant_id);
        CREATE INDEX IF NOT EXISTS idx_orders_device ON orders(device_id);
    )";
    return execute(sql);
}

StatusCode Database::create_recipes_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS recipes (
            id              SERIAL PRIMARY KEY,
            recipe_id       TEXT NOT NULL UNIQUE,
            recipe_name     TEXT NOT NULL,
            device_type     INTEGER NOT NULL DEFAULT 1,
            category        TEXT NOT NULL DEFAULT '',
            is_active       BOOLEAN NOT NULL DEFAULT TRUE,
            version         INTEGER NOT NULL DEFAULT 1,
            description     TEXT NOT NULL DEFAULT ''
        );
    )";
    return execute(sql);
}

StatusCode Database::create_faults_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS faults (
            id              SERIAL PRIMARY KEY,
            tenant_id       TEXT NOT NULL DEFAULT 'default',
            device_id       TEXT NOT NULL,
            code            INTEGER NOT NULL,
            level           INTEGER NOT NULL DEFAULT 1,
            description     TEXT NOT NULL DEFAULT '',
            created_at      BIGINT NOT NULL DEFAULT 0,
            resolved        BOOLEAN NOT NULL DEFAULT FALSE,
            sensor_snapshot TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_faults_device ON faults(tenant_id, device_id);
    )";
    return execute(sql);
}

StatusCode Database::create_firmwares_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS firmwares (
            id              SERIAL PRIMARY KEY,
            version         TEXT NOT NULL UNIQUE,
            product_id      TEXT NOT NULL DEFAULT '',
            download_url    TEXT NOT NULL DEFAULT '',
            checksum_sha256 TEXT NOT NULL DEFAULT '',
            changelog       TEXT NOT NULL DEFAULT '',
            force_upgrade   BOOLEAN NOT NULL DEFAULT FALSE,
            created_at      BIGINT NOT NULL DEFAULT 0
        );
    )";
    return execute(sql);
}

StatusCode Database::create_organizations_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS organizations (
            id              SERIAL PRIMARY KEY,
            org_id          INTEGER NOT NULL UNIQUE,
            parent_id       INTEGER NOT NULL DEFAULT 0,
            tenant_id       TEXT NOT NULL UNIQUE,
            org_name        TEXT NOT NULL,
            org_type        TEXT NOT NULL DEFAULT 'department',
            contact_name    TEXT NOT NULL DEFAULT '',
            contact_phone   TEXT NOT NULL DEFAULT '',
            contact_email   TEXT NOT NULL DEFAULT '',
            address         TEXT NOT NULL DEFAULT '',
            is_active       BOOLEAN NOT NULL DEFAULT TRUE,
            level           INTEGER NOT NULL DEFAULT 0,
            path            TEXT NOT NULL DEFAULT '',
            children_count  INTEGER NOT NULL DEFAULT 0,
            created_at      TEXT NOT NULL DEFAULT '',
            updated_at      TEXT NOT NULL DEFAULT ''
        );
    )";
    return execute(sql);
}

StatusCode Database::create_accounts_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS accounts (
            id              SERIAL PRIMARY KEY,
            account_id      INTEGER NOT NULL UNIQUE,
            username        TEXT NOT NULL UNIQUE,
            display_name    TEXT NOT NULL DEFAULT '',
            password_hash   TEXT NOT NULL DEFAULT '',
            role_code       TEXT NOT NULL DEFAULT 'viewer',
            org_id          INTEGER NOT NULL DEFAULT 0,
            org_name        TEXT NOT NULL DEFAULT '',
            email           TEXT NOT NULL DEFAULT '',
            phone           TEXT NOT NULL DEFAULT '',
            is_active       BOOLEAN NOT NULL DEFAULT TRUE,
            last_login_at   TEXT NOT NULL DEFAULT '',
            created_at      TEXT NOT NULL DEFAULT '',
            updated_at      TEXT NOT NULL DEFAULT ''
        );
    )";
    return execute(sql);
}

StatusCode Database::create_mqtt_tenant_config_table() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS mqtt_tenant_config (
            id              SERIAL PRIMARY KEY,
            tenant_key      TEXT NOT NULL DEFAULT '',
            tenant_name     TEXT NOT NULL DEFAULT ''
        );
        -- Ensure there's always exactly one row
        INSERT INTO mqtt_tenant_config (id, tenant_key, tenant_name)
        SELECT 1, '', '' WHERE NOT EXISTS (SELECT 1 FROM mqtt_tenant_config WHERE id=1);
    )";
    return execute(sql);
}

// ============================================================
// MQTT Tenant Config (persisted to DB instead of text files)
// ============================================================
std::string Database::load_tenant_config(const std::string& field) {
    std::ostringstream sql;
    sql << "SELECT " << field << " FROM mqtt_tenant_config WHERE id=1";
    std::vector<std::vector<std::string>> rows;
    if (query(sql.str(), rows) == StatusCode::OK && !rows.empty() && !rows[0].empty())
        return rows[0][0];
    return "";
}

StatusCode Database::save_tenant_config(const std::string& key, const std::string& name) {
    std::ostringstream sql;
    sql << "UPDATE mqtt_tenant_config SET tenant_key='" << key
        << "', tenant_name='" << name << "' WHERE id=1";
    return execute(sql.str());
}

// ============================================================
// Schema Migration
// ============================================================
StatusCode Database::migrate_if_needed() {
#ifdef HAS_LIBPQ
    // Create schema_version table
    execute(R"(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER NOT NULL DEFAULT 0
        );
    )");

    std::vector<std::vector<std::string>> rows;
    StatusCode sc = query("SELECT MAX(version) FROM schema_version", rows);
    int current_version = 0;
    if (sc == StatusCode::OK && !rows.empty() && !rows[0].empty() && !rows[0][0].empty()) {
        current_version = std::stoi(rows[0][0]);
    }

    std::cout << "[DB] Current schema version: " << current_version << std::endl;

    if (current_version < 1) {
        std::cout << "[DB] Running migration v1: initial schema" << std::endl;
        execute("INSERT INTO schema_version (version) VALUES (1)");
        current_version = 1;
    }

    if (current_version < 2) {
        std::cout << "[DB] Running migration v2: add model_key column" << std::endl;
        execute("ALTER TABLE device_models ADD COLUMN IF NOT EXISTS model_key TEXT NOT NULL DEFAULT ''");
        execute("CREATE INDEX IF NOT EXISTS idx_dm_key ON device_models(model_key)");
        execute("INSERT INTO schema_version (version) VALUES (2)");
        current_version = 2;
    }

    std::cout << "[DB] Schema migration complete. Version: " << current_version << std::endl;
#else
    std::cout << "[DB] In-memory mode: schema migration skipped." << std::endl;
#endif
    return StatusCode::OK;
}

// ============================================================
// Generic SQL execute
// ============================================================
StatusCode Database::execute(const std::string& sql) {
    if (!impl_->opened) return StatusCode::STORAGE_WRITE_ERROR;

#ifdef HAS_LIBPQ
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->exec_cmd(sql.c_str())) {
        return StatusCode::STORAGE_WRITE_ERROR;
    }
    return StatusCode::OK;
#else
    // In-memory stub: accept all DDL silently
    if (sql.find("CREATE TABLE") == 0 || sql.find("CREATE INDEX") == 0 ||
        sql.find("INSERT") == 0 || sql.find("UPDATE") == 0 ||
        sql.find("DELETE") == 0 || sql.find("ALTER") == 0) {
        return StatusCode::OK;
    }
    return StatusCode::OK;
#endif
}

// ============================================================
// Generic SQL query (returns rows)
// ============================================================
StatusCode Database::query(const std::string& sql,
                            std::vector<std::vector<std::string>>& rows) {
    if (!impl_->opened) return StatusCode::STORAGE_READ_ERROR;

#ifdef HAS_LIBPQ
    std::lock_guard<std::mutex> lock(impl_->mutex);
    PGresult* res = PQexec(impl_->conn, sql.c_str());
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK) {
        std::cerr << "[DB] Query failed: " << PQresultErrorMessage(res)
                  << " [SQL: " << sql.substr(0, 120) << "]" << std::endl;
        PQclear(res);
        return StatusCode::STORAGE_READ_ERROR;
    }

    int nrows = PQntuples(res);
    int ncols = PQnfields(res);
    rows.reserve(nrows);
    for (int r = 0; r < nrows; r++) {
        std::vector<std::string> row;
        row.reserve(ncols);
        for (int c = 0; c < ncols; c++) {
            if (PQgetisnull(res, r, c)) {
                row.push_back("");
            } else {
                row.push_back(std::string(PQgetvalue(res, r, c)));
            }
        }
        rows.push_back(std::move(row));
    }
    PQclear(res);
    return StatusCode::OK;
#else
    // In-memory stub: return empty
    rows.clear();
    return StatusCode::OK;
#endif
}

// ============================================================
// Row → struct converters (unchanged column mapping)
// ============================================================
Device Database::device_from_row(const std::vector<std::string>& row) const {
    Device dev;
    // Cols: id, device_id, tenant_id, product_id, device_type,
    //       status, firmware_version, mac_address, model, hardware_uid,
    //       last_heartbeat_at, activated, activated_at, created_at, updated_at
    if (row.size() < 11) return dev;
    dev.device_id         = row[1];
    dev.tenant_id         = row[2];
    dev.product_id        = row[3];
    dev.type              = static_cast<DeviceType>(std::stoi(row[4]));
    dev.network_status    = static_cast<NetworkStatus>(std::stoi(row[5]));
    dev.work_status       = WorkStatus::IDLE;  // default — work_status in separate column
    dev.firmware_version  = row[6];
    dev.mac_address       = row[7];
    dev.model             = row[8];
    dev.hardware_uid      = row[9];
    dev.last_heartbeat_at = row.size() > 10 ? std::stoll(row[10]) : 0;
    dev.activated         = row.size() > 11 ? (row[11] == "t" || row[11] == "true" || row[11] == "TRUE" || row[11] == "1") : false;
    dev.activated_at      = row.size() > 12 ? std::stoll(row[12]) : 0;
    return dev;
}

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
    info.is_active     = row.size() > 7 ? (row[7] == "t" || row[7] == "true" || row[7] == "TRUE" || row[7] == "1") : true;
    info.created_at    = row.size() > 8 ? row[8] : "";
    info.updated_at    = row.size() > 9 ? row[9] : "";
    return info;
}

DeviceModelInfo Database::model_from_row(const std::vector<std::string>& row) const {
    DeviceModelInfo info;
    if (row.size() < 12) return info;
    // Explicit column order: id,model_code,model_key,type_code,display_name,
    //   description,specs_json,firmware_base,is_active,sort_order,created_at,updated_at
    info.id            = std::stoi(row[0]);
    info.model_code    = row[1];
    info.model_key     = row[2];
    info.type_code     = row[3];
    info.display_name  = row[4];
    info.description   = row[5];
    info.specs_json    = row[6];
    info.firmware_base = row[7];
    info.is_active     = (row[8] == "t" || row[8] == "true" || row[8] == "TRUE" || row[8] == "1");
    info.sort_order    = std::stoi(row[9]);
    info.created_at    = row[10];
    info.updated_at    = row[11];
    return info;
}

Order Database::order_from_row(const std::vector<std::string>& row) const {
    Order o;
    // DB column order: id(0),order_id(1),tenant_id(2),device_id(3),
    //   recipe_id(4),cup_size(5),quantity(6),total_amount(7),
    //   payment_method(8),status(9),failure_reason(11),created_at(12),recipe_name(13)
    if (row.size() < 6) return o;
    o.order_id       = row[1];
    o.tenant_id      = row[2];
    o.device_id      = row[3];
    o.recipe_id      = row[4];
    o.cup_size       = row[5];
    auto safe_stoi = [](const std::string& s) -> int { try { return s.empty() ? 0 : std::stoi(s); } catch(...) { return 0; } };
    o.quantity       = row.size() > 6 ? safe_stoi(row[6]) : 0;
    o.total_amount   = row.size() > 7 ? safe_stoi(row[7]) : 0;
    o.payment_method = row.size() > 8 ? row[8] : "";
    o.status         = static_cast<OrderStatus>(row.size() > 9 ? safe_stoi(row[9]) : 0);
    o.failure_reason = row.size() > 11 ? row[11] : "";
    o.created_at     = row.size() > 12 ? row[12] : "0";
    o.recipe_name    = row.size() > 13 ? row[13] : "";
    return o;
}

Recipe Database::recipe_from_row(const std::vector<std::string>& row) const {
    Recipe r;
    if (row.size() < 8) return r;
    r.recipe_id   = row[1];
    r.recipe_name = row[2];
    r.device_type = static_cast<DeviceType>(std::stoi(row[3]));
    r.category    = row[4];
    r.is_active   = (row[5] == "t" || row[5] == "true" || row[5] == "TRUE" || row[5] == "1");
    r.version     = std::stoi(row[6]);
    r.description = row[7];
    return r;
}

FaultInfo Database::fault_from_row(const std::vector<std::string>& row) const {
    FaultInfo f;
    if (row.size() < 7) return f;
    f.tenant_id      = row[1];
    f.device_id      = row[2];
    f.code           = static_cast<FaultCode>(std::stoi(row[3]));
    // Legacy mapping: old L3/L4→ERROR, L0-L2→WARNING
    { int ol = std::stoi(row[4]); f.level = (ol >= 3) ? FaultLevel::ERROR : FaultLevel::WARNING; }
    f.description    = row[5];
    // row[6] is resolved (BOOLEAN), skip
    f.sensor_snapshot = row.size() > 7 ? row[7] : "";
    f.timestamp      = row.size() > 8 ? row[8] : "0";
    return f;
}

FirmwareVersion Database::firmware_from_row(const std::vector<std::string>& row) const {
    FirmwareVersion fw;
    if (row.size() < 7) return fw;
    fw.version         = row[1];
    fw.product_id      = row[2];
    fw.download_url    = row[3];
    fw.checksum_sha256 = row[4];
    fw.changelog       = row[5];
    fw.force_upgrade   = (row[6] == "t" || row[6] == "true" || row[6] == "TRUE" || row[6] == "1");
    fw.created_at      = std::stoll(row[7]);
    return fw;
}

OrgInfo Database::org_from_row(const std::vector<std::string>& row) const {
    OrgInfo info;
    if (row.size() < 15) return info;
    info.org_id        = std::stoi(row[1]);
    info.parent_id     = std::stoi(row[2]);
    info.tenant_id     = row[3];
    info.org_name      = row[4];
    info.org_type      = row[5];
    info.contact_name  = row[6];
    info.contact_phone = row[7];
    info.contact_email = row[8];
    info.address       = row[9];
    info.is_active     = (row[10] == "t" || row[10] == "true" || row[10] == "TRUE" || row[10] == "1");
    info.level         = std::stoi(row[11]);
    info.path          = row[12];
    info.children_count = std::stoi(row[13]);
    info.created_at    = row[14];
    info.updated_at    = row[15];
    return info;
}

AccountInfo Database::account_from_row(const std::vector<std::string>& row) const {
    AccountInfo info;
    if (row.size() < 13) return info;
    info.account_id    = std::stoi(row[1]);
    info.username      = row[2];
    info.display_name  = row[3];
    info.password_hash = row[4];
    info.role_code     = row[5];
    info.org_id        = std::stoi(row[6]);
    info.org_name      = row[7];
    info.email         = row[8];
    info.phone         = row[9];
    info.is_active     = (row[10] == "t" || row[10] == "true" || row[10] == "TRUE" || row[10] == "1");
    info.last_login_at = row[11];
    info.created_at    = row[12];
    info.updated_at    = row[13];
    return info;
}

// ============================================================
// Device CRUD
// ============================================================
StatusCode Database::insert_device(const Device& device) {
    std::ostringstream sql;
    sql << "INSERT INTO devices "
        << "(device_id, tenant_id, product_id, device_type, status, "
        << "firmware_version, mac_address, model, hardware_uid, "
        << "last_heartbeat_at, activated, activated_at) VALUES ("
        << sql_str(device.device_id) << ","
        << sql_str(device.tenant_id) << ","
        << sql_str(device.product_id) << ","
        << static_cast<int>(device.type) << ","
        << static_cast<int>(device.network_status) << ","
        << sql_str(device.firmware_version) << ","
        << sql_str(device.mac_address) << ","
        << sql_str(device.model) << ","
        << sql_str(device.hardware_uid) << ","
        << device.last_heartbeat_at << ","
        << sql_bool(device.activated) << ","
        << device.activated_at
        << ")";
    return execute(sql.str());
}

StatusCode Database::update_device(const Device& device) {
    std::ostringstream sql;
    sql << "UPDATE devices SET "
        << "tenant_id=" << sql_str(device.tenant_id) << ","
        << "network_status=" << static_cast<int>(device.network_status) << ","
        << "work_status=" << static_cast<int>(device.work_status) << ","
        << "firmware_version=" << sql_str(device.firmware_version) << ","
        << "last_heartbeat_at=" << device.last_heartbeat_at << ","
        << "activated=" << sql_bool(device.activated) << ","
        << "activated_at=" << device.activated_at << ","
        << "updated_at=EXTRACT(EPOCH FROM NOW())::BIGINT "
        << "WHERE device_id=" << sql_str(device.device_id);
    return execute(sql.str());
}

StatusCode Database::upsert_device(const Device& device) {
    std::ostringstream sql;
    sql << "INSERT INTO devices "
        << "(device_id, tenant_id, product_id, device_type, status, "
        << "firmware_version, mac_address, model, hardware_uid, "
        << "last_heartbeat_at, activated, activated_at) VALUES ("
        << sql_str(device.device_id) << ","
        << sql_str(device.tenant_id) << ","
        << sql_str(device.product_id) << ","
        << static_cast<int>(device.type) << ","
        << static_cast<int>(device.network_status) << ","
        << sql_str(device.firmware_version) << ","
        << sql_str(device.mac_address) << ","
        << sql_str(device.model) << ","
        << sql_str(device.hardware_uid) << ","
        << device.last_heartbeat_at << ","
        << sql_bool(device.activated) << ","
        << device.activated_at
        << ") ON CONFLICT (device_id) DO UPDATE SET "
        << "tenant_id=EXCLUDED.tenant_id,"
        << "status=EXCLUDED.status,"
        << "firmware_version=EXCLUDED.firmware_version,"
        << "last_heartbeat_at=EXCLUDED.last_heartbeat_at,"
        << "updated_at=EXTRACT(EPOCH FROM NOW())::BIGINT";
    return execute(sql.str());
}

std::optional<Device> Database::get_device(const std::string& device_id) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM devices WHERE device_id=" + sql_str(device_id);
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;
    return device_from_row(rows[0]);
}

std::optional<Device> Database::get_device_by_hwuid(const std::string& hardware_uid) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM devices WHERE hardware_uid=" + sql_str(hardware_uid);
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;
    return device_from_row(rows[0]);
}

std::vector<Device> Database::list_devices_by_tenant(const std::string& tenant_id) {
    std::vector<Device> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM devices WHERE tenant_id=" + sql_str(tenant_id), rows);
    for (const auto& row : rows) result.push_back(device_from_row(row));
    return result;
}

std::vector<Device> Database::list_all_devices() {
    std::vector<Device> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM devices", rows);
    for (const auto& row : rows) result.push_back(device_from_row(row));
    return result;
}

bool Database::device_exists(const std::string& hardware_uid) const {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT COUNT(*) FROM devices WHERE hardware_uid=" + sql_str(hardware_uid);
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
    sql << "INSERT INTO activation_tokens "
        << "(device_id, token, issued_at, expires_at, revoked) VALUES ("
        << sql_str(device_id) << ","
        << sql_str(token) << ","
        << issued_at << ","
        << expires_at << ","
        << "FALSE"
        << ") ON CONFLICT (device_id) DO UPDATE SET "
        << "token=EXCLUDED.token, issued_at=EXCLUDED.issued_at, "
        << "expires_at=EXCLUDED.expires_at, revoked=FALSE";
    return execute(sql.str());
}

std::optional<std::string> Database::get_activation_token(const std::string& device_id) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT token, expires_at, revoked FROM activation_tokens WHERE device_id="
                      + sql_str(device_id);
    StatusCode sc = query(sql, rows);
    if (sc != StatusCode::OK || rows.empty()) return std::nullopt;

    if (rows[0].size() >= 3) {
        bool revoked = (rows[0][2] == "t" || rows[0][2] == "true" || rows[0][2] == "TRUE" || rows[0][2] == "1");
        int64_t expires = std::stoll(rows[0][1]);
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
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
        << (device_id.empty() ? "NULL" : sql_str(device_id)) << ","
        << sql_str("hardware_uid=" + hardware_uid + " msg=" + message) << ","
        << sql_str(remote_ip) << ","
        << sql_bool(success)
        << ")";
    return execute(sql.str());
}

// ============================================================
// Device Types CRUD
// ============================================================
StatusCode Database::insert_device_type(const DeviceTypeInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO device_types "
        << "(type_code, display_name, description, internal_type, icon_url, sort_order) VALUES ("
        << sql_str(info.type_code) << "," << sql_str(info.display_name) << ","
        << sql_str(info.description) << "," << info.internal_type << ","
        << sql_str(info.icon_url) << "," << info.sort_order << ")";
    return execute(sql.str());
}

StatusCode Database::update_device_type(const DeviceTypeInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE device_types SET display_name=" << sql_str(info.display_name)
        << ",description=" << sql_str(info.description)
        << ",internal_type=" << info.internal_type
        << ",icon_url=" << sql_str(info.icon_url)
        << ",sort_order=" << info.sort_order
        << ",is_active=" << sql_bool(info.is_active)
        << ",updated_at=NOW()::TEXT WHERE type_code=" << sql_str(info.type_code);
    return execute(sql.str());
}

StatusCode Database::delete_device_type(const std::string& type_code) {
    return execute("UPDATE device_types SET is_active=FALSE, updated_at=NOW()::TEXT "
                   "WHERE type_code=" + sql_str(type_code));
}

std::optional<DeviceTypeInfo> Database::get_device_type(const std::string& type_code) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM device_types WHERE type_code=" + sql_str(type_code), rows);
    if (rows.empty()) return std::nullopt;
    return type_from_row(rows[0]);
}

std::vector<DeviceTypeInfo> Database::list_device_types(bool active_only) {
    std::vector<DeviceTypeInfo> result;
    std::string sql = "SELECT * FROM device_types";
    if (active_only) sql += " WHERE is_active=TRUE";
    sql += " ORDER BY sort_order ASC";
    std::vector<std::vector<std::string>> rows;
    if (query(sql, rows) == StatusCode::OK)
        for (const auto& r : rows) result.push_back(type_from_row(r));
    return result;
}

// ============================================================
// Device Models CRUD
// ============================================================
StatusCode Database::insert_device_model(const DeviceModelInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO device_models "
        << "(model_code, model_key, type_code, display_name, description, specs_json, firmware_base, sort_order) VALUES ("
        << sql_str(info.model_code) << "," << sql_str(info.model_key) << ","
        << sql_str(info.type_code) << "," << sql_str(info.display_name) << ","
        << sql_str(info.description) << "," << sql_str(info.specs_json) << ","
        << sql_str(info.firmware_base) << "," << info.sort_order << ")";
    return execute(sql.str());
}

StatusCode Database::update_device_model(const DeviceModelInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE device_models SET model_key=" << sql_str(info.model_key)
        << ",type_code=" << sql_str(info.type_code)
        << ",display_name=" << sql_str(info.display_name)
        << ",description=" << sql_str(info.description)
        << ",specs_json=" << sql_str(info.specs_json)
        << ",firmware_base=" << sql_str(info.firmware_base)
        << ",is_active=" << sql_bool(info.is_active)
        << ",sort_order=" << info.sort_order
        << ",updated_at=NOW()::TEXT WHERE model_code=" << sql_str(info.model_code);
    return execute(sql.str());
}

StatusCode Database::delete_device_model(const std::string& model_code) {
    return execute("UPDATE device_models SET is_active=FALSE, updated_at=NOW()::TEXT "
                   "WHERE model_code=" + sql_str(model_code));
}

std::optional<DeviceModelInfo> Database::get_device_model_by_key(const std::string& model_key) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT id,model_code,model_key,type_code,display_name,description,"
          "specs_json,firmware_base,is_active,sort_order,created_at,updated_at "
          "FROM device_models WHERE model_key=" + sql_str(model_key), rows);
    if (rows.empty()) return std::nullopt;
    return model_from_row(rows[0]);
}

std::optional<DeviceModelInfo> Database::get_device_model(const std::string& model_code) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT id,model_code,model_key,type_code,display_name,description,"
          "specs_json,firmware_base,is_active,sort_order,created_at,updated_at "
          "FROM device_models WHERE model_code=" + sql_str(model_code), rows);
    if (rows.empty()) return std::nullopt;
    return model_from_row(rows[0]);
}

std::vector<DeviceModelInfo> Database::list_device_models(const std::string& type_code, bool active_only) {
    std::vector<DeviceModelInfo> result;
    std::string sql = "SELECT id,model_code,model_key,type_code,display_name,description,"
                      "specs_json,firmware_base,is_active,sort_order,created_at,updated_at "
                      "FROM device_models WHERE 1=1";
    if (!type_code.empty()) sql += " AND type_code=" + sql_str(type_code);
    if (active_only) sql += " AND is_active=TRUE";
    sql += " ORDER BY type_code, sort_order ASC";
    std::vector<std::vector<std::string>> rows;
    if (query(sql, rows) == StatusCode::OK)
        for (const auto& r : rows) result.push_back(model_from_row(r));
    return result;
}

// ============================================================
// Orders CRUD
// ============================================================
StatusCode Database::insert_order(const Order& order) {
    std::ostringstream sql;
    sql << "INSERT INTO orders "
        << "(order_id,tenant_id,device_id,recipe_id,recipe_name,cup_size,quantity,total_amount,"
        << "payment_method,status,created_at,failure_reason) VALUES ("
        << sql_str(order.order_id) << "," << sql_str(order.tenant_id) << ","
        << sql_str(order.device_id) << "," << sql_str(order.recipe_id) << ","
        << sql_str(order.recipe_name) << ","
        << sql_str(order.cup_size) << "," << order.quantity << ","
        << order.total_amount << "," << sql_str(order.payment_method) << ","
        << static_cast<int>(order.status) << "," << order.created_at << ","
        << sql_str(order.failure_reason)
        << ") ON CONFLICT (order_id) DO UPDATE SET "
        << "tenant_id=EXCLUDED.tenant_id,device_id=EXCLUDED.device_id,"
        << "recipe_id=EXCLUDED.recipe_id,recipe_name=EXCLUDED.recipe_name,cup_size=EXCLUDED.cup_size,"
        << "quantity=EXCLUDED.quantity,total_amount=EXCLUDED.total_amount,"
        << "payment_method=EXCLUDED.payment_method,status=EXCLUDED.status,"
        << "created_at=EXCLUDED.created_at,failure_reason=EXCLUDED.failure_reason";
    return execute(sql.str());
}

StatusCode Database::update_order_status(const std::string& order_id, int status) {
    return execute("UPDATE orders SET status=" + std::to_string(status)
                   + " WHERE order_id=" + sql_str(order_id));
}

std::optional<Order> Database::get_order(const std::string& order_id) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM orders WHERE order_id=" + sql_str(order_id), rows);
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

// ============================================================
// Recipes CRUD
// ============================================================
StatusCode Database::insert_recipe(const Recipe& recipe) {
    std::ostringstream sql;
    sql << "INSERT INTO recipes "
        << "(recipe_id,recipe_name,device_type,category,is_active,version,description) VALUES ("
        << sql_str(recipe.recipe_id) << "," << sql_str(recipe.recipe_name) << ","
        << static_cast<int>(recipe.device_type) << "," << sql_str(recipe.category) << ","
        << sql_bool(recipe.is_active) << "," << recipe.version << ","
        << sql_str(recipe.description)
        << ") ON CONFLICT (recipe_id) DO UPDATE SET "
        << "recipe_name=EXCLUDED.recipe_name,device_type=EXCLUDED.device_type,"
        << "category=EXCLUDED.category,is_active=EXCLUDED.is_active,"
        << "version=EXCLUDED.version,description=EXCLUDED.description";
    return execute(sql.str());
}

StatusCode Database::update_recipe(const Recipe& recipe) { return insert_recipe(recipe); }

StatusCode Database::delete_recipe(const std::string& recipe_id) {
    return execute("UPDATE recipes SET is_active=FALSE WHERE recipe_id=" + sql_str(recipe_id));
}

std::optional<Recipe> Database::get_recipe(const std::string& recipe_id) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM recipes WHERE recipe_id=" + sql_str(recipe_id), rows);
    if (!rows.empty()) return recipe_from_row(rows[0]);
    return std::nullopt;
}

std::vector<Recipe> Database::list_all_recipes() {
    std::vector<Recipe> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM recipes WHERE is_active=TRUE", rows);
    for (const auto& row : rows) result.push_back(recipe_from_row(row));
    return result;
}

// ============================================================
// Faults CRUD
// ============================================================
StatusCode Database::insert_fault(const FaultInfo& fault) {
    std::ostringstream sql;
    sql << "INSERT INTO faults "
        << "(tenant_id,device_id,code,level,description,created_at,sensor_snapshot) VALUES ("
        << sql_str(fault.tenant_id) << "," << sql_str(fault.device_id) << ","
        << static_cast<int>(fault.code) << "," << static_cast<int>(fault.level) << ","
        << sql_str(fault.description) << "," << fault.timestamp << ","
        << sql_str(fault.sensor_snapshot) << ")";
    return execute(sql.str());
}

StatusCode Database::resolve_fault(const std::string& tenant_id, const std::string& device_id, int code) {
    std::ostringstream sql;
    sql << "UPDATE faults SET resolved=TRUE WHERE tenant_id=" << sql_str(tenant_id)
        << " AND device_id=" << sql_str(device_id)
        << " AND code=" << code;
    return execute(sql.str());
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
    query("SELECT * FROM faults WHERE resolved=FALSE ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(fault_from_row(row));
    return result;
}

std::vector<FaultInfo> Database::list_faults_by_device(const std::string& tenant_id,
                                                         const std::string& device_id) {
    std::vector<FaultInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM faults WHERE tenant_id=" + sql_str(tenant_id)
          + " AND device_id=" + sql_str(device_id) + " ORDER BY id DESC", rows);
    for (const auto& row : rows) result.push_back(fault_from_row(row));
    return result;
}

// ============================================================
// Firmwares CRUD
// ============================================================
StatusCode Database::insert_firmware(const FirmwareVersion& fw) {
    std::ostringstream sql;
    sql << "INSERT INTO firmwares "
        << "(version,product_id,download_url,checksum_sha256,changelog,force_upgrade,created_at) VALUES ("
        << sql_str(fw.version) << "," << sql_str(fw.product_id) << ","
        << sql_str(fw.download_url) << "," << sql_str(fw.checksum_sha256) << ","
        << sql_str(fw.changelog) << "," << sql_bool(fw.force_upgrade) << ","
        << fw.created_at
        << ") ON CONFLICT (version) DO UPDATE SET "
        << "product_id=EXCLUDED.product_id,download_url=EXCLUDED.download_url,"
        << "checksum_sha256=EXCLUDED.checksum_sha256,changelog=EXCLUDED.changelog,"
        << "force_upgrade=EXCLUDED.force_upgrade,created_at=EXCLUDED.created_at";
    return execute(sql.str());
}

StatusCode Database::delete_firmware(const std::string& version) {
    return execute("DELETE FROM firmwares WHERE version=" + sql_str(version));
}

std::optional<FirmwareVersion> Database::get_firmware(const std::string& version) {
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM firmwares WHERE version=" + sql_str(version), rows);
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

// ============================================================
// Organizations CRUD
// ============================================================
StatusCode Database::insert_org(const OrgInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO organizations "
        << "(org_id,parent_id,tenant_id,org_name,org_type,contact_name,contact_phone,contact_email,address,is_active,level,path,children_count,created_at,updated_at) VALUES ("
        << info.org_id << "," << info.parent_id << ","
        << sql_str(info.tenant_id) << "," << sql_str(info.org_name) << ","
        << sql_str(info.org_type) << "," << sql_str(info.contact_name) << ","
        << sql_str(info.contact_phone) << "," << sql_str(info.contact_email) << ","
        << sql_str(info.address) << "," << sql_bool(info.is_active) << ","
        << info.level << "," << sql_str(info.path) << ","
        << info.children_count << "," << sql_str(info.created_at) << ","
        << sql_str(info.updated_at) << ")";
    return execute(sql.str());
}

StatusCode Database::update_org_db(const OrgInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE organizations SET "
        << "parent_id=" << info.parent_id << ","
        << "tenant_id=" << sql_str(info.tenant_id) << ","
        << "org_name=" << sql_str(info.org_name) << ","
        << "org_type=" << sql_str(info.org_type) << ","
        << "contact_name=" << sql_str(info.contact_name) << ","
        << "contact_phone=" << sql_str(info.contact_phone) << ","
        << "contact_email=" << sql_str(info.contact_email) << ","
        << "address=" << sql_str(info.address) << ","
        << "is_active=" << sql_bool(info.is_active) << ","
        << "level=" << info.level << ","
        << "path=" << sql_str(info.path) << ","
        << "children_count=" << info.children_count << ","
        << "updated_at=" << sql_str(info.updated_at)
        << " WHERE org_id=" << info.org_id;
    return execute(sql.str());
}

StatusCode Database::delete_org_db(int32_t org_id) {
    std::ostringstream sql;
    sql << "DELETE FROM organizations WHERE org_id=" << org_id;
    return execute(sql.str());
}

std::vector<OrgInfo> Database::list_orgs_db() {
    std::vector<OrgInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM organizations ORDER BY org_id ASC", rows);
    for (const auto& row : rows) result.push_back(org_from_row(row));
    return result;
}

// ============================================================
// Accounts CRUD
// ============================================================
StatusCode Database::insert_account(const AccountInfo& info) {
    std::ostringstream sql;
    sql << "INSERT INTO accounts "
        << "(account_id,username,display_name,password_hash,role_code,org_id,org_name,email,phone,is_active,last_login_at,created_at,updated_at) VALUES ("
        << info.account_id << "," << sql_str(info.username) << ","
        << sql_str(info.display_name) << "," << sql_str(info.password_hash) << ","
        << sql_str(info.role_code) << "," << info.org_id << ","
        << sql_str(info.org_name) << "," << sql_str(info.email) << ","
        << sql_str(info.phone) << "," << sql_bool(info.is_active) << ","
        << sql_str(info.last_login_at) << "," << sql_str(info.created_at) << ","
        << sql_str(info.updated_at) << ")";
    return execute(sql.str());
}

StatusCode Database::update_account_db(const AccountInfo& info) {
    std::ostringstream sql;
    sql << "UPDATE accounts SET "
        << "display_name=" << sql_str(info.display_name) << ","
        << "role_code=" << sql_str(info.role_code) << ","
        << "org_id=" << info.org_id << ","
        << "org_name=" << sql_str(info.org_name) << ","
        << "email=" << sql_str(info.email) << ","
        << "phone=" << sql_str(info.phone) << ","
        << "is_active=" << sql_bool(info.is_active) << ","
        << "updated_at=" << sql_str(info.updated_at)
        << " WHERE account_id=" << info.account_id;
    return execute(sql.str());
}

StatusCode Database::delete_account_db(int32_t account_id) {
    std::ostringstream sql;
    sql << "DELETE FROM accounts WHERE account_id=" << account_id;
    return execute(sql.str());
}

std::vector<AccountInfo> Database::list_accounts_db() {
    std::vector<AccountInfo> result;
    std::vector<std::vector<std::string>> rows;
    query("SELECT * FROM accounts ORDER BY account_id ASC", rows);
    for (const auto& row : rows) result.push_back(account_from_row(row));
    return result;
}

// ============================================================
// Maintenance
// ============================================================
StatusCode Database::vacuum() {
    // PostgreSQL auto-vacuums; manual VACUUM is optional
    return execute("VACUUM");
}

StatusCode Database::clean_expired_data(int retention_days) {
    std::ostringstream sql;
    sql << "DELETE FROM audit_log WHERE created_at < "
        << "EXTRACT(EPOCH FROM NOW())::BIGINT - " << (retention_days * 86400);
    execute(sql.str());

    // Revoke expired tokens
    sql.str("");
    sql << "UPDATE activation_tokens SET revoked=TRUE WHERE expires_at < "
        << "EXTRACT(EPOCH FROM NOW())::BIGINT";
    execute(sql.str());

    return StatusCode::OK;
}

StatusCode Database::backup(const std::string& backup_path) {
#ifdef HAS_LIBPQ
    // Use pg_dump via PQ (or just file copy for simple backup)
    std::ostringstream cmd;
    cmd << "pg_dump " << impl_->conn_string << " > " << backup_path;
    int rc = system(cmd.str().c_str());
    if (rc != 0) {
        std::cerr << "[DB] pg_dump failed with exit code " << rc << std::endl;
        return StatusCode::STORAGE_WRITE_ERROR;
    }
    return StatusCode::OK;
#else
    std::cerr << "[DB] Backup requires libpq" << std::endl;
    return StatusCode::ERROR;
#endif
}

} // namespace dev_sys
