cmake_minimum_required(VERSION 3.16)
project(laya VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization Flags
add_compile_options(-O3 -Wall -Wextra -pthread)

# Include directories
include_directories(
    include
    include/laya
    /usr/include/onnxruntime
    /usr/include/poppler/cpp
)

# Dependencies
find_package(Threads REQUIRED)

# Embedded TinyXML2
add_library(tinyxml2 STATIC src/tinyxml2.cpp)
target_include_directories(tinyxml2 PUBLIC include include/laya)

# System Libraries
find_library(ONNXRUNTIME_LIB NAMES onnxruntime)
find_library(POPPLER_CPP_LIB NAMES poppler-cpp)
find_library(RE2_LIB NAMES re2)

# libarchive Static or Shared
find_library(ARCHIVE_STATIC_LIB NAMES libarchive.a PATHS /usr/lib/x86_64-linux-gnu /usr/lib)
if (ARCHIVE_STATIC_LIB)
    set(ARCHIVE_LIBRARIES ${ARCHIVE_STATIC_LIB} nettle acl lzma zstd lz4 bz2 z xml2)
else()
    find_library(ARCHIVE_DYN_LIB NAMES archive)
    set(ARCHIVE_LIBRARIES ${ARCHIVE_DYN_LIB})
endif()

# Executables
add_executable(laya_daemon src/daemon.cpp)
target_link_libraries(laya_daemon PRIVATE tinyxml2 ${ONNXRUNTIME_LIB} ${POPPLER_CPP_LIB} ${RE2_LIB} ${ARCHIVE_LIBRARIES} Threads::Threads)

add_executable(laya_client src/client.cpp)
target_link_libraries(laya_client PRIVATE Threads::Threads)

add_executable(benchmark_client src/benchmark_client.cpp)
target_link_libraries(benchmark_client PRIVATE ${ONNXRUNTIME_LIB} ${POPPLER_CPP_LIB} ${RE2_LIB} ${ARCHIVE_LIBRARIES} Threads::Threads)

add_executable(benchmark_per_core src/benchmark_per_core.cpp)
target_link_libraries(benchmark_per_core PRIVATE ${ONNXRUNTIME_LIB} ${POPPLER_CPP_LIB} ${RE2_LIB} ${ARCHIVE_LIBRARIES} Threads::Threads)

# Installation targets
install(TARGETS laya_daemon laya_client benchmark_client benchmark_per_core DESTINATION bin)
install(FILES config/laya.xml config/laya_section.xml DESTINATION /etc/laya)
install(FILES systemd/laya-daemon.service DESTINATION /lib/systemd/system)
