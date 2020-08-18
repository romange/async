cmake_minimum_required(VERSION 3.4)

# package information
set(PACKAGE_NAME      "async_uring")
set(PROJECT_CONTACT romange@gmail.com)

project(${PACKAGE_NAME})

option (ONLY_THIRD_PARTY "Build third party only" OFF)
option (BUILD_DOCS "Generate documentation " ON)



Message("PROJECT_BINARY_DIR ${PROJECT_BINARY_DIR} GENERATOR ${CMAKE_GENERATOR}")

set(CMAKE_CXX_STANDARD 17)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(third_party)
include(internal)

add_subdirectory(base)
