add_library(dev_hal_sensor STATIC
    temp_sensor.cpp
    flow_sensor.cpp
)
target_include_directories(dev_hal_sensor PUBLIC ${CMAKE_SOURCE_DIR}/include)
