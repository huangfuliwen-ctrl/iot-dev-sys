add_library(dev_app_fault STATIC
    fault_detector.cpp
)
target_include_directories(dev_app_fault PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_fault PRIVATE dev_middleware_comm)
