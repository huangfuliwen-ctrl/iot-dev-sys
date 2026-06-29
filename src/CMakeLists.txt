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
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(dev-sys-cloud PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        add_compile_definitions(HAS_OPENSSL)
        message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    else()
        message(WARNING "OpenSSL not found — TLS features will be stubbed")
    endif()
endif()

if(USE_SYSTEM_CURL)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(dev-sys-cloud PRIVATE CURL::libcurl)
        add_compile_definitions(HAS_CURL)
        message(STATUS "libcurl found: ${CURL_VERSION}")
    else()
        message(WARNING "libcurl not found — HTTPS fallback will be stubbed")
    endif()
endif()

if(USE_SYSTEM_SQLITE)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        target_link_libraries(dev-sys-cloud PRIVATE SQLite::SQLite3)
        add_compile_definitions(HAS_SQLITE)
        message(STATUS "SQLite3 found: ${SQLite3_VERSION}")
    else()
        message(WARNING "SQLite3 not found — using in-memory storage")
    endif()
endif()

if(USE_SYSTEM_LIBPQ)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBPQ IMPORTED_TARGET libpq)
        if(LIBPQ_FOUND)
            target_link_libraries(dev-sys-cloud PRIVATE PkgConfig::LIBPQ)
            add_compile_definitions(HAS_LIBPQ)
            message(STATUS "libpq found: ${LIBPQ_VERSION}")
        else()
            message(WARNING "libpq not found via pkg-config.")
        endif()
    else()
        find_path(LIBPQ_INCLUDE_DIR libpq-fe.h)
        find_library(LIBPQ_LIBRARY pq)
        if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
            target_include_directories(dev-sys-cloud PRIVATE ${LIBPQ_INCLUDE_DIR})
            target_link_libraries(dev-sys-cloud PRIVATE ${LIBPQ_LIBRARY})
            add_compile_definitions(HAS_LIBPQ)
            message(STATUS "libpq found: ${LIBPQ_LIBRARY}")
        else()
            message(WARNING "libpq not found.")
        endif()
    endif()
endif()

if(USE_SYSTEM_PAHO)
    set(PAHO_FOUND 0)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PAHO_MQTTCPP IMPORTED_TARGET paho-mqttpp3)
        if(PAHO_MQTTCPP_FOUND)
            target_link_libraries(dev-sys-cloud PRIVATE PkgConfig::PAHO_MQTTCPP)
            add_compile_definitions(HAS_PAHO)
            message(STATUS "Paho MQTT C++ (pkg-config): ${PAHO_MQTTCPP_VERSION}")
            set(PAHO_FOUND 1)
        endif()
    endif()
    if(NOT PAHO_FOUND)
        find_path(PAHO_CPP_INCLUDE_DIR mqtt/async_client.h
            HINTS /usr/include /usr/local/include)
        find_library(PAHO_CPP_LIBRARY
            NAMES paho-mqttpp3 libpaho-mqttpp3.so.1)
        find_library(PAHO_C_LIBRARY
            NAMES paho-mqtt3as libpaho-mqtt3as.so.1)
        if(PAHO_CPP_INCLUDE_DIR AND PAHO_CPP_LIBRARY AND PAHO_C_LIBRARY)
            target_include_directories(dev-sys-cloud PRIVATE ${PAHO_CPP_INCLUDE_DIR})
            target_link_libraries(dev-sys-cloud PRIVATE ${PAHO_CPP_LIBRARY} ${PAHO_C_LIBRARY})
            add_compile_definitions(HAS_PAHO)
            message(STATUS "Paho MQTT C++ (manual): ${PAHO_CPP_LIBRARY}")
        else()
            message(WARNING "Paho MQTT C++ not found. MQTT will be stubbed.")
        endif()
    endif()
endif()

# Install
install(TARGETS dev-sys-cloud RUNTIME DESTINATION bin)
