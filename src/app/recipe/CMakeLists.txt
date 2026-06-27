add_library(dev_app_recipe STATIC
    recipe_manager.cpp
)
target_include_directories(dev_app_recipe PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_recipe PRIVATE dev_middleware_storage)
