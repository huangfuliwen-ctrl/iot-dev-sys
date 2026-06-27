# Test targets (require GoogleTest)
find_package(GTest QUIET)
if(NOT GTEST_FOUND)
    message(STATUS "GoogleTest not found, skipping tests")
    return()
endif()

include(GoogleTest)

# App layer tests
add_executable(test_device_manager
    app/test_device_manager.cpp
)
target_link_libraries(test_device_manager PRIVATE dev_app_device dev_middleware_storage GTest::gtest_main)
gtest_discover_tests(test_device_manager)

add_executable(test_order_manager
    app/test_order_manager.cpp
)
target_link_libraries(test_order_manager PRIVATE dev_app_order GTest::gtest_main)
gtest_discover_tests(test_order_manager)

add_executable(test_ota_manager
    app/test_ota_manager.cpp
)
target_link_libraries(test_ota_manager PRIVATE dev_app_ota GTest::gtest_main)
gtest_discover_tests(test_ota_manager)

add_executable(test_fault_detector
    app/test_fault_detector.cpp
)
target_link_libraries(test_fault_detector PRIVATE dev_app_fault GTest::gtest_main)
gtest_discover_tests(test_fault_detector)

# Middleware tests
add_executable(test_mqtt_client
    middleware/test_mqtt_client.cpp
)
target_link_libraries(test_mqtt_client PRIVATE dev_middleware_comm GTest::gtest_main)
gtest_discover_tests(test_mqtt_client)

add_executable(test_database
    middleware/test_database.cpp
)
target_link_libraries(test_database PRIVATE dev_middleware_storage GTest::gtest_main)
gtest_discover_tests(test_database)

add_executable(test_log_manager
    middleware/test_log_manager.cpp
)
target_link_libraries(test_log_manager PRIVATE dev_middleware_storage GTest::gtest_main)
gtest_discover_tests(test_log_manager)
