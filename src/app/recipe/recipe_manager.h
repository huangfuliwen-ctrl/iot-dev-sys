#pragma once

#include "dev_sys/common/types.h"
#include "dev_sys/common/status_codes.h"
#include <memory>
#include <vector>
#include <string>

namespace dev_sys {

// ============================================================
// Recipe Manager (REQ-RC-001)
// ============================================================
class RecipeManager {
public:
    RecipeManager();
    ~RecipeManager();

    // Load all recipes for current device type
    StatusCode load_recipes(DeviceType device_type);

    // Sync recipes from cloud (incremental)
    StatusCode sync_from_cloud();

    // Query
    std::vector<Recipe> all_recipes() const;
    std::optional<Recipe> find_by_id(const std::string& recipe_id) const;

    // Local cache management
    int32_t recipe_count() const;
    StatusCode create_recipe(const Recipe& recipe);
    StatusCode update_recipe(const Recipe& recipe);
    StatusCode remove_recipe(const std::string& recipe_id);

    // Mock data for frontend development
    void seed_mock_data();
    void set_database(class Database* db) { db_ = db; }
    StatusCode load_from_database();

private:
    struct Impl;
    Database* db_ = nullptr;
    std::unique_ptr<Impl> impl_;
};

} // namespace dev_sys
