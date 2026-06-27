add_library(dev_middleware_security STATIC
    tls_manager.cpp
)
target_include_directories(dev_middleware_security PUBLIC ${CMAKE_SOURCE_DIR}/include)
