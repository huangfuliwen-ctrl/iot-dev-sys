add_library(dev_hal_peripheral STATIC
    display.cpp
    bluetooth.cpp
)
target_include_directories(dev_hal_peripheral PUBLIC ${CMAKE_SOURCE_DIR}/include)
