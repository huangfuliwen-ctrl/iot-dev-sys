add_library(dev_hal_actuator STATIC
    heater.cpp
    pump.cpp
    motor.cpp
    valve.cpp
)
target_include_directories(dev_hal_actuator PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_hal_actuator PRIVATE dev_hal_sensor)
