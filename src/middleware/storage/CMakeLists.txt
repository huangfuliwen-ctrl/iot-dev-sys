add_library(dev_middleware_storage STATIC
    database.cpp
    log_manager.cpp
)
target_include_directories(dev_middleware_storage PUBLIC ${CMAKE_SOURCE_DIR}/include)
