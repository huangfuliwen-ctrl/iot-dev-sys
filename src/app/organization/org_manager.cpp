#include "org_manager.h"
#include "../../middleware/storage/database.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>

namespace dev_sys {

// ============================================================
// 同步租户到 MQTT Broker
// ============================================================
static void sync_tenant_to_broker(const std::string& tenant_id) {
    const char* api_url = std::getenv("MQTT_BROKER_API");
    if (!api_url || !api_url[0]) return; // not configured, skip

    std::string url(api_url);
    // Parse host:port from http://host:port
    std::string host = "127.0.0.1"; int port = 8080;
    if (url.find("http://") == 0) url = url.substr(7);
    size_t c = url.find(':'); size_t s = url.find('/');
    host = (c != std::string::npos) ? url.substr(0, c) : url.substr(0, s);
    if (c != std::string::npos) port = std::stoi(url.substr(c + 1, s - c - 1));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    timeval tv{3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    addrinfo h{}, *res;
    h.ai_family = AF_INET; h.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &h, &res) != 0) {
        close(fd); return;
    }
    sockaddr_in a{}; memcpy(&a, res->ai_addr, sizeof(a)); freeaddrinfo(res);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { close(fd); return; }

    std::string body = "{\"tenant_id\":\"" + tenant_id + "\"}";
    std::ostringstream req;
    req << "PUT /api/v1/tenants/" << tenant_id << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n" << body;
    std::string rs = req.str();
    send(fd, rs.c_str(), rs.size(), 0);

    char buf[4096]; recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    std::cout << "[OrgMgr] Synced tenant '" << tenant_id << "' to MQTT broker " << host << ":" << port << std::endl;
}

static void delete_tenant_from_broker(const std::string& tenant_id) {
    const char* api_url = std::getenv("MQTT_BROKER_API");
    if (!api_url || !api_url[0]) return;
    std::string url(api_url); if (url.find("http://") == 0) url = url.substr(7);
    std::string host = "127.0.0.1"; int port = 8080;
    size_t c = url.find(':'); size_t s = url.find('/');
    host = (c != std::string::npos) ? url.substr(0, c) : url.substr(0, s);
    if (c != std::string::npos) port = std::stoi(url.substr(c + 1, s - c - 1));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    timeval tv{3, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    addrinfo h{}, *res; h.ai_family = AF_INET; h.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &h, &res) != 0) { close(fd); return; }
    sockaddr_in a{}; memcpy(&a, res->ai_addr, sizeof(a)); freeaddrinfo(res);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { close(fd); return; }

    std::ostringstream req;
    req << "DELETE /api/v1/tenants/" << tenant_id << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string rs = req.str();
    send(fd, rs.c_str(), rs.size(), 0);
    char buf[4096]; recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    std::cout << "[OrgMgr] Deleted tenant '" << tenant_id << "' from MQTT broker" << std::endl;
}

OrgManager::OrgManager() = default;
OrgManager::~OrgManager() = default;

StatusCode OrgManager::load_from_database() {
    if (!db_) return StatusCode::STORAGE_READ_ERROR;
    auto db_orgs = db_->list_orgs_db();
    if (db_orgs.empty()) return StatusCode::OK; // nothing to load
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int32_t max_id = 0;
    for (const auto& o : db_orgs) {
        orgs_.push_back(o);
        if (o.org_id > max_id) max_id = o.org_id;
    }
    next_id_ = max_id + 1;
    std::cout << "[OrgMgr] Loaded " << orgs_.size() << " organizations from database" << std::endl;
    return StatusCode::OK;
}

// ============================================================
// CRUD
// ============================================================
StatusCode OrgManager::create_org(OrgInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Validate parent
    if (info.parent_id != 0) {
        auto parent = get_org(info.parent_id);
        if (!parent) return StatusCode::ORG_NOT_FOUND;
        if (!parent->is_active) return StatusCode::ORG_NOT_FOUND;
        // Company can only have department children
        if (parent->org_type == "company" && info.org_type != "department")
            return StatusCode::ERROR;
    }

    // Validate tenant_id uniqueness
    if (tenant_exists(info.tenant_id))
        return StatusCode::ORG_DUPLICATE_TENANT;

    // Assign ID
    info.org_id = next_id_++;

    // Compute level and path
    if (info.parent_id == 0) {
        info.level = 0;
        info.path = "/" + std::to_string(info.org_id);
    } else {
        auto parent = get_org(info.parent_id);
        info.level = parent->level + 1;
        info.path = parent->path + "/" + std::to_string(info.org_id);
    }

    // Timestamps
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    info.created_at = ts.str();
    info.updated_at = ts.str();

    orgs_.push_back(info);

    // Persist to database
    if (db_) db_->insert_org(info);

    // Update parent's children_count
    if (info.parent_id != 0) {
        for (auto& o : orgs_) {
            if (o.org_id == info.parent_id) {
                o.children_count++;
                break;
            }
        }
    }

    std::cout << "[OrgMgr] Created org: " << info.org_name
              << " (id=" << info.org_id << " tenant=" << info.tenant_id
              << " path=" << info.path << ")" << std::endl;
    sync_tenant_to_broker(info.tenant_id);
    return StatusCode::OK;
}

StatusCode OrgManager::update_org(int32_t org_id, const OrgInfo& info) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = std::find_if(orgs_.begin(), orgs_.end(),
        [org_id](const OrgInfo& o) { return o.org_id == org_id; });
    if (it == orgs_.end()) return StatusCode::ORG_NOT_FOUND;

    // Only allow updating certain fields
    if (!info.org_name.empty()) it->org_name = info.org_name;
    if (!info.contact_name.empty()) it->contact_name = info.contact_name;
    if (!info.contact_phone.empty()) it->contact_phone = info.contact_phone;
    if (!info.contact_email.empty()) it->contact_email = info.contact_email;
    if (!info.address.empty()) it->address = info.address;
    // is_active can be toggled
    it->is_active = info.is_active;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    it->updated_at = ts.str();

    // Sync to database
    if (db_) db_->update_org_db(*it);

    return StatusCode::OK;
}

StatusCode OrgManager::delete_org(int32_t org_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = std::find_if(orgs_.begin(), orgs_.end(),
        [org_id](const OrgInfo& o) { return o.org_id == org_id; });
    if (it == orgs_.end()) return StatusCode::ORG_NOT_FOUND;

    // Check children
    if (has_children(org_id)) return StatusCode::ORG_HAS_CHILDREN;

    // Soft delete
    it->is_active = false;
    if (db_) db_->update_org_db(*it);
    delete_tenant_from_broker(it->tenant_id);
    return StatusCode::OK;
}

// ============================================================
// Query
// ============================================================
std::optional<OrgInfo> OrgManager::get_org(int32_t org_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = std::find_if(orgs_.begin(), orgs_.end(),
        [org_id](const OrgInfo& o) { return o.org_id == org_id; });
    if (it != orgs_.end()) return *it;
    return std::nullopt;
}

std::optional<OrgInfo> OrgManager::get_org_by_tenant(const std::string& tenant_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = std::find_if(orgs_.begin(), orgs_.end(),
        [&tenant_id](const OrgInfo& o) { return o.tenant_id == tenant_id; });
    if (it != orgs_.end()) return *it;
    return std::nullopt;
}

std::vector<OrgInfo> OrgManager::list_orgs(int32_t parent_id,
                                            const std::string& org_type,
                                            bool active_only) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<OrgInfo> result;
    for (const auto& o : orgs_) {
        if (active_only && !o.is_active) continue;
        if (parent_id >= 0 && o.parent_id != parent_id) continue;
        if (!org_type.empty() && o.org_type != org_type) continue;
        result.push_back(o);
    }
    return result;
}

OrgTreeNode OrgManager::get_org_tree() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    OrgTreeNode root;
    // Build the tree array directly from root nodes
    for (const auto& o : orgs_) {
        if (o.parent_id == 0 && o.is_active) {
            OrgTreeNode node;
            node.info = o;
            auto child_nodes = build_tree(o.org_id);
            for (auto& c : child_nodes) node.children.push_back(std::move(c));
            root.children.push_back(std::move(node));
        }
    }
    return root;
}

std::vector<OrgTreeNode> OrgManager::build_tree(int32_t parent_id) const {
    std::vector<OrgTreeNode> result;
    for (const auto& o : orgs_) {
        if (o.parent_id == parent_id && o.is_active) {
            OrgTreeNode node;
            node.info = o;
            auto child_nodes = build_tree(o.org_id);
            for (auto& c : child_nodes) node.children.push_back(std::move(c));
            result.push_back(std::move(node));
        }
    }
    return result;
}

std::vector<OrgInfo> OrgManager::get_descendants(int32_t org_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<OrgInfo> result;
    auto self = get_org(org_id);
    if (!self) return result;

    std::string prefix = self->path + "/";
    for (const auto& o : orgs_) {
        if (o.path.find(prefix) == 0) result.push_back(o);
    }
    return result;
}

std::vector<int32_t> OrgManager::get_org_scope(int32_t org_id) const {
    std::vector<int32_t> scope;
    scope.push_back(org_id);
    auto descendants = get_descendants(org_id);
    for (const auto& d : descendants) {
        scope.push_back(d.org_id);
    }
    return scope;
}

int32_t OrgManager::org_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return static_cast<int32_t>(orgs_.size());
}

bool OrgManager::org_exists(int32_t org_id) const {
    return get_org(org_id).has_value();
}

bool OrgManager::tenant_exists(const std::string& tenant_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return std::any_of(orgs_.begin(), orgs_.end(),
        [&tenant_id](const OrgInfo& o) { return o.tenant_id == tenant_id; });
}

bool OrgManager::has_children(int32_t org_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return std::any_of(orgs_.begin(), orgs_.end(),
        [org_id](const OrgInfo& o) { return o.parent_id == org_id && o.is_active; });
}

// ============================================================
// Mock data
// ============================================================
void OrgManager::seed_mock_data() {
    // Company
    OrgInfo company;
    company.parent_id = 0;
    company.tenant_id = "company_zhangsan";
    company.org_name = "张三咖啡有限公司";
    company.org_type = "company";
    company.contact_name = "张三";
    company.contact_phone = "13800001111";
    company.contact_email = "zhangsan@example.com";
    company.address = "上海市浦东新区张江高科技园区";
    create_org(company);

    // Department 1
    OrgInfo dept1;
    dept1.parent_id = 1;
    dept1.tenant_id = "dept_huadong";
    dept1.org_name = "华东区";
    dept1.org_type = "department";
    dept1.contact_name = "李经理";
    dept1.contact_phone = "13900002222";
    dept1.contact_email = "lijingli@example.com";
    create_org(dept1);

    // Sub-department under 华东区
    OrgInfo sub1;
    sub1.parent_id = 2;
    sub1.tenant_id = "dept_shanghai";
    sub1.org_name = "上海运营中心";
    sub1.org_type = "department";
    sub1.contact_name = "王五";
    sub1.contact_phone = "13600003333";
    sub1.contact_email = "wangwu@example.com";
    create_org(sub1);

    OrgInfo sub2;
    sub2.parent_id = 2;
    sub2.tenant_id = "dept_hangzhou";
    sub2.org_name = "杭州运营中心";
    sub2.org_type = "department";
    sub2.contact_name = "赵六";
    sub2.contact_phone = "13500004444";
    sub2.contact_email = "zhaoliu@example.com";
    create_org(sub2);

    // Department 2
    OrgInfo dept2;
    dept2.parent_id = 1;
    dept2.tenant_id = "dept_huanan";
    dept2.org_name = "华南区";
    dept2.org_type = "department";
    dept2.contact_name = "陈总监";
    dept2.contact_phone = "13700005555";
    dept2.contact_email = "chen@example.com";
    create_org(dept2);

    // Department 3
    OrgInfo dept3;
    dept3.parent_id = 1;
    dept3.tenant_id = "dept_rnd";
    dept3.org_name = "研发部";
    dept3.org_type = "department";
    dept3.contact_name = "孙工";
    dept3.contact_phone = "13300006666";
    dept3.contact_email = "sun@example.com";
    create_org(dept3);

    std::cout << "[OrgMgr] Seeded " << orgs_.size() << " organizations" << std::endl;
}

} // namespace dev_sys
