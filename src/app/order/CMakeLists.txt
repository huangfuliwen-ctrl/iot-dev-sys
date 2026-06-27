add_library(dev_app_order STATIC
    order_manager.cpp
)
target_include_directories(dev_app_order PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dev_app_order PRIVATE dev_middleware_comm dev_middleware_storage)
