add_library(dev_middleware_storage STATIC
    database.cpp
    log_manager.cpp
)
target_include_directories(dev_middleware_storage PUBLIC ${CMAKE_SOURCE_DIR}/include)

# libpq include path (PostgreSQL)
if(USE_SYSTEM_LIBPQ)
    find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
        HINTS /usr/include/postgresql
              /usr/local/include/postgresql
              /usr/local/include
    )
    if(LIBPQ_INCLUDE_DIR)
        target_include_directories(dev_middleware_storage PUBLIC ${LIBPQ_INCLUDE_DIR})
    endif()
endif()
