#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>
#include <mutex>

namespace dev_sys {

// ============================================================
// Org Manager — 多层级组织架构管理 (REQ-ORG-001/003)
// ============================================================
class OrgManager {
public:
    OrgManager();
    ~OrgManager();

    // CRUD
    StatusCode create_org(OrgInfo& info);          // assigns org_id, level, path
    StatusCode update_org(int32_t org_id, const OrgInfo& info);
    StatusCode delete_org(int32_t org_id);

    // Query
    std::optional<OrgInfo> get_org(int32_t org_id) const;
    std::optional<OrgInfo> get_org_by_tenant(const std::string& tenant_id) const;
    std::vector<OrgInfo> list_orgs(int32_t parent_id = -1,
                                    const std::string& org_type = "",
                                    bool active_only = false) const;
    OrgTreeNode get_org_tree() const;
    std::vector<OrgInfo> get_descendants(int32_t org_id) const;
    std::vector<int32_t> get_org_scope(int32_t org_id) const; // org + all descendants
    int32_t org_count() const;

    // Validation
    bool org_exists(int32_t org_id) const;
    bool tenant_exists(const std::string& tenant_id) const;
    bool has_children(int32_t org_id) const;

    // Mock data for frontend development
    void seed_mock_data();

private:
    std::vector<OrgTreeNode> build_tree(int32_t parent_id) const;

    mutable std::recursive_mutex mutex_;
    std::vector<OrgInfo> orgs_;
    int32_t next_id_ = 1;
};

} // namespace dev_sys
