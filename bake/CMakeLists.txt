cmake_minimum_required(VERSION 3.2)

find_package (PkgConfig REQUIRED)
find_package(yokan REQUIRED)
pkg_check_modules (MARGO REQUIRED IMPORTED_TARGET margo)
pkg_check_modules (ABT REQUIRED IMPORTED_TARGET argobots)
pkg_check_modules (ABTIO REQUIRED IMPORTED_TARGET abt-io)
pkg_check_modules (SSG REQUIRED IMPORTED_TARGET ssg)
pkg_check_modules (BAKECLIENT REQUIRED IMPORTED_TARGET bake-client)
pkg_check_modules (BAKESERVER REQUIRED IMPORTED_TARGET bake-server)
find_package(Boost REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(bake_writer writer.cc)
target_link_libraries(bake_writer PkgConfig::BAKECLIENT PkgConfig::BAKESERVER yokan-admin yokan-client yokan-server)
