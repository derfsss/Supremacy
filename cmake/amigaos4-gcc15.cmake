# CMake toolchain: AmigaOS 4 (PowerPC) with the adtools gcc 15 cross compiler
# and newlib, as in the Docker image walkero/amigagccondocker:os4-gcc15
# (ppc-amigaos-gcc 15.3.0; SDL3 3.4.18 for newlib in the SDK).
# Used by tools/build_os4.sh.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ppc)
set(AMIGAOS4 TRUE)

set(OS4_SDK /opt/ppc-amigaos/ppc-amigaos/SDK)
set(CMAKE_C_COMPILER ppc-amigaos-gcc)
set(CMAKE_AR ppc-amigaos-ar)
set(CMAKE_RANLIB ppc-amigaos-ranlib)

set(CMAKE_C_FLAGS_INIT "-mcrt=newlib -D__AMIGAOS4__")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcrt=newlib")

set(CMAKE_FIND_ROOT_PATH ${OS4_SDK}/local/newlib ${OS4_SDK}/newlib)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
