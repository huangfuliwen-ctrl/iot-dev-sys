# App layer subdirectories
add_subdirectory(device)
add_subdirectory(recipe)
add_subdirectory(order)
add_subdirectory(ota)
add_subdirectory(fault)
add_subdirectory(config)
add_subdirectory(organization)

# Message Router (cloud-side core)
add_library(dev_app_router STATIC
    message_router.cpp
)
target_include_directories(dev_app_router PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_router PRIVATE dev_app_device dev_app_order dev_app_ota dev_app_fault)
