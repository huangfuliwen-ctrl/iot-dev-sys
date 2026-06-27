# Cloud platform service — no HAL needed
# Build main executable and link all sub-libraries

# Subdirectories (must be processed before linking)
add_subdirectory(app)
add_subdirectory(middleware/communication)
add_subdirectory(middleware/security)
add_subdirectory(middleware/storage)

# HAL subdirectories are NOT included (cloud service doesn't control hardware)
# add_subdirectory(hal/sensor)
# add_subdirectory(hal/actuator)
# add_subdirectory(hal/peripheral)

# Collect all source files
set(APP_SOURCES
    main.cpp
)

add_executable(dev-sys-cloud ${APP_SOURCES})

# Link sub-libraries
target_link_libraries(dev-sys-cloud
    PRIVATE
        dev_app_device
        dev_app_recipe
        dev_app_order
        dev_app_ota
        dev_app_fault
        dev_app_config
        dev_app_org
        dev_app_router
        dev_middleware_comm
        dev_middleware_security
        dev_middleware_storage
    PUBLIC
        Threads::Threads
)

# External dependencies (conditional for embedded builds)
if(USE_SYSTEM_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(dev-sys-cloud PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(USE_SYSTEM_CURL)
    find_package(CURL REQUIRED)
    target_link_libraries(dev-sys-cloud PRIVATE CURL::libcurl)
endif()

if(USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)
    target_link_libraries(dev-sys-cloud PRIVATE SQLite::SQLite3)
endif()

# Install
install(TARGETS dev-sys-cloud RUNTIME DESTINATION bin)
