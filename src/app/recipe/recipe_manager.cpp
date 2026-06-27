#include "../../middleware/storage/database.h"
#include "recipe_manager.h"
#include <algorithm>
#include <iostream>

namespace dev_sys {

struct RecipeManager::Impl {
    std::vector<Recipe> recipes;
    DeviceType device_type = DeviceType::OTHER;
};

RecipeManager::RecipeManager()
    : impl_(std::make_unique<Impl>()) {}

RecipeManager::~RecipeManager() = default;

StatusCode RecipeManager::load_recipes(DeviceType device_type) {
    impl_->device_type = device_type;
    // TODO: Load recipes from local SQLite database
    // Filter by device_type compatibility
    return StatusCode::OK;
}

StatusCode RecipeManager::sync_from_cloud() {
    // TODO: GET /api/v1/recipes/sync with last sync version
    // TODO: Incrementally update local DB with changed recipes
    // TODO: Don't affect ongoing brewing tasks
    return StatusCode::OK;
}

std::vector<Recipe> RecipeManager::all_recipes() const {
    return impl_->recipes;
}

std::optional<Recipe> RecipeManager::find_by_id(const std::string& recipe_id) const {
    auto it = std::find_if(impl_->recipes.begin(), impl_->recipes.end(),
        [&recipe_id](const Recipe& r) { return r.recipe_id == recipe_id; });
    if (it != impl_->recipes.end()) {
        return *it;
    }
    return std::nullopt;
}

int32_t RecipeManager::recipe_count() const {
    return static_cast<int32_t>(impl_->recipes.size());
}

StatusCode RecipeManager::create_recipe(const Recipe& recipe) {
    auto it = std::find_if(impl_->recipes.begin(), impl_->recipes.end(),
        [&recipe](const Recipe& r) { return r.recipe_id == recipe.recipe_id; });
    if (it != impl_->recipes.end()) {
        return StatusCode::ORDER_INVALID_STATE; // duplicate recipe_id
    }
    impl_->recipes.push_back(recipe);
    return StatusCode::OK;
}

StatusCode RecipeManager::update_recipe(const Recipe& recipe) {
    auto it = std::find_if(impl_->recipes.begin(), impl_->recipes.end(),
        [&recipe](const Recipe& r) { return r.recipe_id == recipe.recipe_id; });
    if (it != impl_->recipes.end()) {
        *it = recipe;
    } else {
        impl_->recipes.push_back(recipe);
    }
    return StatusCode::OK;
}

StatusCode RecipeManager::remove_recipe(const std::string& recipe_id) {
    impl_->recipes.erase(
        std::remove_if(impl_->recipes.begin(), impl_->recipes.end(),
            [&recipe_id](const Recipe& r) { return r.recipe_id == recipe_id; }),
        impl_->recipes.end());
    return StatusCode::OK;
}

void RecipeManager::seed_mock_data() {
    // Americano
    Recipe r1;
    r1.recipe_id = "REC-AMERICANO-001";
    r1.recipe_name = "经典美式";
    r1.device_type = DeviceType::COFFEE_MACHINE;
    r1.category = "咖啡";
    r1.is_active = true;
    r1.version = 3;
    r1.steps = {
        {"grind", 5000, {{"bean_weight_g", 15.0}}},
        {"brew", 25000, {{"temperature_c", 92.0}, {"pressure_bar", 9.0}, {"volume_ml", 200.0}}},
        {"dispense", 3000, {}}
    };
    r1.cup_sizes = {{"小", 1200, 200}, {"中", 1500, 300}, {"大", 1800, 400}};
    impl_->recipes.push_back(r1);

    // Latte
    Recipe r2;
    r2.recipe_id = "REC-LATTE-001";
    r2.recipe_name = "拿铁咖啡";
    r2.device_type = DeviceType::COFFEE_MACHINE;
    r2.category = "咖啡";
    r2.is_active = true;
    r2.version = 5;
    r2.steps = {
        {"grind", 5000, {{"bean_weight_g", 15.0}}},
        {"brew", 25000, {{"temperature_c", 92.0}, {"pressure_bar", 9.0}, {"volume_ml", 40.0}}},
        {"heat", 10000, {{"temperature_c", 65.0}, {"volume_ml", 200.0}}},
        {"mix", 3000, {}},
        {"dispense", 3000, {}}
    };
    r2.cup_sizes = {{"小", 1800, 240}, {"中", 2200, 300}, {"大", 2600, 400}};
    impl_->recipes.push_back(r2);

    // Cappuccino
    Recipe r3;
    r3.recipe_id = "REC-CAPPUCCINO-001";
    r3.recipe_name = "卡布奇诺";
    r3.device_type = DeviceType::COFFEE_MACHINE;
    r3.category = "咖啡";
    r3.is_active = true;
    r3.version = 4;
    r3.steps = {
        {"grind", 5000, {{"bean_weight_g", 15.0}}},
        {"brew", 25000, {{"temperature_c", 92.0}, {"volume_ml", 40.0}}},
        {"heat", 8000, {{"temperature_c", 65.0}, {"volume_ml", 120.0}}},
        {"mix", 5000, {}},
        {"dispense", 3000, {}}
    };
    r3.cup_sizes = {{"小", 2000, 240}, {"中", 2400, 300}, {"大", 2800, 400}};
    impl_->recipes.push_back(r3);

    // Mocha
    Recipe r4;
    r4.recipe_id = "REC-MOCHA-001";
    r4.recipe_name = "摩卡咖啡";
    r4.device_type = DeviceType::COFFEE_MACHINE;
    r4.category = "咖啡";
    r4.is_active = true;
    r4.version = 2;
    r4.steps = {
        {"grind", 5000, {{"bean_weight_g", 15.0}}},
        {"brew", 25000, {{"temperature_c", 92.0}, {"volume_ml", 40.0}}},
        {"mix", 5000, {{"chocolate_syrup_ml", 20.0}}},
        {"heat", 8000, {{"temperature_c", 65.0}, {"volume_ml", 180.0}}},
        {"dispense", 3000, {}}
    };
    r4.cup_sizes = {{"小", 2400, 240}, {"中", 2800, 300}, {"大", 3200, 400}};
    impl_->recipes.push_back(r4);

    // Hot Water (for water dispenser)
    Recipe r5;
    r5.recipe_id = "REC-HOTWATER-001";
    r5.recipe_name = "热水";
    r5.device_type = DeviceType::WATER_DISPENSER;
    r5.category = "热水";
    r5.is_active = true;
    r5.version = 1;
    r5.steps = {
        {"heat", 15000, {{"temperature_c", 95.0}, {"volume_ml", 300.0}}},
        {"dispense", 3000, {}}
    };
    r5.cup_sizes = {{"小", 300, 200}, {"中", 500, 300}, {"大", 800, 500}};
    impl_->recipes.push_back(r5);

    // Instant milk tea (for instant machine)
    Recipe r6;
    r6.recipe_id = "REC-MILKTEA-001";
    r6.recipe_name = "原味奶茶";
    r6.device_type = DeviceType::INSTANT_MACHINE;
    r6.category = "茶饮";
    r6.is_active = true;
    r6.version = 2;
    r6.steps = {
        {"mix", 8000, {{"powder_g", 25.0}, {"water_volume_ml", 250.0}, {"temperature_c", 85.0}}},
        {"dispense", 3000, {}}
    };
    r6.cup_sizes = {{"中", 1200, 300}, {"大", 1500, 400}};
    impl_->recipes.push_back(r6);

    std::cout << "[RecipeMgr] Seeded " << impl_->recipes.size() << " mock recipes" << std::endl;
}

StatusCode RecipeManager::load_from_database() {
    if (!db_) return StatusCode::STORAGE_READ_ERROR;
    auto recipes = db_->list_all_recipes();
    impl_->recipes = recipes;
    std::cout << "[RecipeMgr] Loaded " << recipes.size() << " recipes from database" << std::endl;
    return StatusCode::OK;
}
} // namespace dev_sys
