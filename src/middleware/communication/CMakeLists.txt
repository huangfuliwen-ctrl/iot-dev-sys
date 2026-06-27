add_library(dev_middleware_comm STATIC
    mqtt_client.cpp
    http_client.cpp
    api_server.cpp
)
target_include_directories(dev_middleware_comm PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_middleware_comm PRIVATE dev_middleware_security)

# Paho MQTT C++ include path
if(USE_SYSTEM_PAHO)
    find_path(PAHO_INCLUDE_DIR mqtt/async_client.h
        HINTS /usr/include /usr/local/include)
    if(PAHO_INCLUDE_DIR)
        target_include_directories(dev_middleware_comm PUBLIC ${PAHO_INCLUDE_DIR})
    endif()
endif()
