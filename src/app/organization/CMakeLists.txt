add_library(dev_app_org STATIC
    org_manager.cpp
    account_manager.cpp
)
target_include_directories(dev_app_org PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(dev_app_org PRIVATE dev_middleware_storage)
