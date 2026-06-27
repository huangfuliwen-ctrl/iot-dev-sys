add_library(dev_app_device STATIC
    device_manager.cpp
    device_activation.cpp
    device_type_manager.cpp
)
target_include_directories(dev_app_device PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dev_app_device PRIVATE dev_middleware_storage dev_middleware_security)
