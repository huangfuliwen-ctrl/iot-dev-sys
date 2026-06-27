#include "account_manager.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iomanip>
#include <random>
#include <functional>

namespace dev_sys {

// Permission mapping table
static const std::unordered_map<std::string, std::vector<std::string>> ROLE_PERMISSIONS = {
    {"super_admin", {
        "org:read", "org:write",
        "account:read", "account:write",
        "device:read", "device:write",
        "order:read",
        "recipe:read", "recipe:write",
        "ota:read", "ota:write",
        "fault:read", "fault:write",
        "config:read", "config:write"
    }},
    {"org_admin", {
        "org:read", "org:write",
        "account:read", "account:write",
        "device:read", "device:write",
        "order:read",
        "recipe:read", "recipe:write",
        "ota:read", "ota:write",
        "fault:read", "fault:write",
        "config:read"
    }},
    {"dept_admin", {
        "org:read",
        "account:read",
        "device:read", "device:write",
        "order:read",
        "recipe:read",
        "ota:read",
        "fault:read", "fault:write"
    }},
    {"operator", {
        "device:read",
        "order:read",
        "recipe:read", "recipe:write",
        "ota:read",
        "fault:read", "fault:write"
    }},
    {"viewer", {
        "device:read",
        "order:read",
        "recipe:read",
        "ota:read",
        "fault:read"
    }}
};

static const std::vector<std::string> VALID_ROLES = {
    "super_admin", "org_admin", "dept_admin", "operator", "viewer"
};

std::vector<std::string> AccountManager::permissions_for_role(const std::string& role_code) {
    auto it = ROLE_PERMISSIONS.find(role_code);
    if (it != ROLE_PERMISSIONS.end()) return it->second;
    return {};
}

AccountManager::AccountManager(OrgManager* org_mgr)
    : org_mgr_(org_mgr) {}

AccountManager::~AccountManager() = default;

// ============================================================
// CRUD
// ============================================================
StatusCode AccountManager::create_account(AccountInfo& info, const std::string& password) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Validate username uniqueness
    if (get_account_by_username(info.username))
        return StatusCode::ACCOUNT_DUPLICATE;

    // Validate org exists
    if (!org_mgr_->org_exists(info.org_id))
        return StatusCode::ORG_NOT_FOUND;

    // Validate role
    auto it = std::find(VALID_ROLES.begin(), VALID_ROLES.end(), info.role_code);
    if (it == VALID_ROLES.end())
        return StatusCode::ERROR;

    // Assign ID
    info.account_id = next_id_++;
    info.password_hash = hash_password(password);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    info.created_at = ts.str();
    info.updated_at = ts.str();
    info.is_active = true;

    // Denormalize org_name
    auto org = org_mgr_->get_org(info.org_id);
    if (org) info.org_name = org->org_name;

    accounts_.push_back(info);
    std::cout << "[AcctMgr] Created account: " << info.username
              << " (id=" << info.account_id << " role=" << info.role_code << ")" << std::endl;
    return StatusCode::OK;
}

StatusCode AccountManager::update_account(int32_t account_id, const AccountInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = std::find_if(accounts_.begin(), accounts_.end(),
        [account_id](const AccountInfo& a) { return a.account_id == account_id; });
    if (it == accounts_.end()) return StatusCode::ACCOUNT_NOT_FOUND;

    if (!info.display_name.empty()) it->display_name = info.display_name;
    if (!info.email.empty()) it->email = info.email;
    if (!info.phone.empty()) it->phone = info.phone;
    if (!info.role_code.empty()) it->role_code = info.role_code;
    it->is_active = info.is_active;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    it->updated_at = ts.str();

    return StatusCode::OK;
}

StatusCode AccountManager::change_password(int32_t account_id,
                                            const std::string& old_password,
                                            const std::string& new_password) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = std::find_if(accounts_.begin(), accounts_.end(),
        [account_id](const AccountInfo& a) { return a.account_id == account_id; });
    if (it == accounts_.end()) return StatusCode::ACCOUNT_NOT_FOUND;

    if (it->password_hash != hash_password(old_password))
        return StatusCode::ACCOUNT_BAD_PASSWORD;

    it->password_hash = hash_password(new_password);
    return StatusCode::OK;
}

StatusCode AccountManager::delete_account(int32_t account_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = std::find_if(accounts_.begin(), accounts_.end(),
        [account_id](const AccountInfo& a) { return a.account_id == account_id; });
    if (it == accounts_.end()) return StatusCode::ACCOUNT_NOT_FOUND;

    // Soft delete (deactivate)
    it->is_active = false;
    return StatusCode::OK;
}

// ============================================================
// Auth
// ============================================================
LoginResponse AccountManager::login(const LoginRequest& req) {
    LoginResponse resp;

    auto acct = get_account_by_username(req.username);
    if (!acct) {
        resp.error_code = static_cast<int>(StatusCode::ACCOUNT_NOT_FOUND);
        resp.error_message = "Account not found";
        return resp;
    }
    if (!acct->is_active) {
        resp.error_code = static_cast<int>(StatusCode::ACCOUNT_LOCKED);
        resp.error_message = "Account is disabled";
        return resp;
    }
    if (acct->password_hash != hash_password(req.password)) {
        resp.error_code = static_cast<int>(StatusCode::ACCOUNT_BAD_PASSWORD);
        resp.error_message = "Invalid password";
        return resp;
    }

    // Get org scope
    auto org_scope = org_mgr_->get_org_scope(acct->org_id);

    // Generate token
    std::string token = generate_token(*acct, org_scope);

    // Update last login
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    // Find and update in accounts_
    for (auto& a : accounts_) {
        if (a.account_id == acct->account_id) {
            a.last_login_at = ts.str();
            break;
        }
    }

    resp.success = true;
    resp.token = token;
    resp.expires_at = std::to_string(t + 86400); // 24h from now
    resp.account = *acct;
    resp.account.password_hash = ""; // never expose
    resp.permissions = permissions_for_role(acct->role_code);

    std::cout << "[AcctMgr] Login: " << req.username
              << " org_scope=" << org_scope.size() << " orgs" << std::endl;
    return resp;
}

StatusCode AccountManager::logout(const std::string& token) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    invalidated_tokens_.push_back(token);
    return StatusCode::OK;
}

std::optional<TokenPayload> AccountManager::verify_token(const std::string& token) const {
    // Mock JWT verification — in production use a real JWT library
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Check blacklist
    if (std::find(invalidated_tokens_.begin(), invalidated_tokens_.end(), token)
        != invalidated_tokens_.end())
        return std::nullopt;

    // Mock: token format "mock_jwt:account_id:exp_epoch"
    if (token.find("mock_jwt:") != 0) return std::nullopt;

    std::string payload = token.substr(9); // after "mock_jwt:"
    size_t colon = payload.find(':');
    if (colon == std::string::npos) return std::nullopt;

    int32_t account_id = std::stoi(payload.substr(0, colon));
    int64_t exp = std::stoll(payload.substr(colon + 1));

    // Check expiration
    auto now = std::chrono::system_clock::now();
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (now_epoch > exp) return std::nullopt;

    auto acct = get_account(account_id);
    if (!acct || !acct->is_active) return std::nullopt;

    TokenPayload tp;
    tp.account_id = acct->account_id;
    tp.username = acct->username;
    tp.org_id = acct->org_id;
    tp.org_scope = org_mgr_->get_org_scope(acct->org_id);
    tp.role_code = acct->role_code;
    tp.permissions = permissions_for_role(acct->role_code);
    tp.iat = now_epoch;
    tp.exp = exp;

    return tp;
}

// ============================================================
// Query
// ============================================================
std::optional<AccountInfo> AccountManager::get_account(int32_t account_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = std::find_if(accounts_.begin(), accounts_.end(),
        [account_id](const AccountInfo& a) { return a.account_id == account_id; });
    if (it != accounts_.end()) {
        AccountInfo info = *it;
        info.password_hash = ""; // never expose
        return info;
    }
    return std::nullopt;
}

std::optional<AccountInfo> AccountManager::get_account_by_username(const std::string& username) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = std::find_if(accounts_.begin(), accounts_.end(),
        [&username](const AccountInfo& a) { return a.username == username; });
    if (it != accounts_.end()) return *it; // includes hash for internal use
    return std::nullopt;
}

std::vector<AccountInfo> AccountManager::list_accounts(int32_t org_id,
                                                         const std::string& role_code,
                                                         bool active_only) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<AccountInfo> result;
    for (auto a : accounts_) {
        if (active_only && !a.is_active) continue;
        if (org_id >= 0 && a.org_id != org_id) continue;
        if (!role_code.empty() && a.role_code != role_code) continue;
        a.password_hash = ""; // never expose
        result.push_back(a);
    }
    return result;
}

int32_t AccountManager::account_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int32_t>(accounts_.size());
}

// ============================================================
// Token generation (mock)
// ============================================================
std::string AccountManager::generate_token(const AccountInfo& account,
                                            const std::vector<int32_t>& org_scope) {
    (void)org_scope;
    auto now = std::chrono::system_clock::now();
    auto exp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count() + 86400; // 24h
    return "mock_jwt:" + std::to_string(account.account_id) + ":" + std::to_string(exp);
}

// ============================================================
// Password hashing (mock — use bcrypt/argon2 in production)
// ============================================================
std::string AccountManager::hash_password(const std::string& password) const {
    // Mock hash: SHA256-like placeholder, production MUST use bcrypt/argon2
    std::hash<std::string> hasher;
    size_t h = hasher(password);
    std::ostringstream oss;
    oss << "mock_hash:" << std::hex << h;
    return oss.str();
}

// ============================================================
// Mock data
// ============================================================
void AccountManager::seed_mock_data() {
    AccountInfo a1;
    a1.username = "admin";
    a1.display_name = "张三（超级管理员）";
    a1.org_id = 1;
    a1.role_code = "super_admin";
    a1.email = "admin@example.com";
    a1.phone = "13800000000";
    create_account(a1, "Admin@123");

    AccountInfo a2;
    a2.username = "lijingli";
    a2.display_name = "李经理";
    a2.org_id = 2;
    a2.role_code = "org_admin";
    a2.email = "lijingli@example.com";
    a2.phone = "13900002222";
    create_account(a2, "Org@12345");

    AccountInfo a3;
    a3.username = "wangwu";
    a3.display_name = "王五";
    a3.org_id = 3;
    a3.role_code = "dept_admin";
    a3.email = "wangwu@example.com";
    a3.phone = "13600003333";
    create_account(a3, "Dept@12345");

    AccountInfo a4;
    a4.username = "operator1";
    a4.display_name = "运营员小赵";
    a4.org_id = 5;
    a4.role_code = "operator";
    a4.email = "zhao@example.com";
    create_account(a4, "Oper@12345");

    AccountInfo a5;
    a5.username = "viewer1";
    a5.display_name = "只读用户小刘";
    a5.org_id = 6;
    a5.role_code = "viewer";
    a5.email = "liu@example.com";
    create_account(a5, "View@12345");

    std::cout << "[AcctMgr] Seeded " << accounts_.size() << " accounts" << std::endl;
}

} // namespace dev_sys
