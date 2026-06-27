# Cross-compilation toolchain for ARM Cortex-A (Linux)
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/arm-linux-gnueabihf.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Toolchain prefix
set(TOOLCHAIN_PREFIX arm-linux-gnueabihf)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_LINKER       ${TOOLCHAIN_PREFIX}-ld)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}-objcopy)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_PREFIX}-objdump)
set(CMAKE_STRIP        ${TOOLCHAIN_PREFIX}-strip)

# Sysroot (adjust to your SDK path)
set(CMAKE_SYSROOT /opt/arm-sysroot)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Compiler flags
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -march=armv7-a -mthumb -mfpu=neon -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mthumb -mfpu=neon -mfloat-abi=hard")
