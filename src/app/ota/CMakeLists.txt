add_library(dev_app_ota STATIC
    ota_manager.cpp
)
target_include_directories(dev_app_ota PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_ota PRIVATE dev_middleware_comm dev_middleware_security)
