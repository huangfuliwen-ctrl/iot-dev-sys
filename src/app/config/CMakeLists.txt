add_library(dev_app_config STATIC
    config_manager.cpp
)
target_include_directories(dev_app_config PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_config PRIVATE dev_middleware_storage)
