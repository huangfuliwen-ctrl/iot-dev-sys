add_library(dev_middleware_comm STATIC
    mqtt_client.cpp
    http_client.cpp
    api_server.cpp
)
target_include_directories(dev_middleware_comm PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_middleware_comm PRIVATE dev_middleware_security)
