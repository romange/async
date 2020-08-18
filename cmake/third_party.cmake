set(THIRD_PARTY_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party")

SET_DIRECTORY_PROPERTIES(PROPERTIES EP_PREFIX ${THIRD_PARTY_DIR})

Include(ExternalProject)
Include(FetchContent)


set(THIRD_PARTY_LIB_DIR "${THIRD_PARTY_DIR}/libs")
file(MAKE_DIRECTORY ${THIRD_PARTY_LIB_DIR})

set(THIRD_PARTY_CXX_FLAGS "-std=c++14 -O3 -DNDEBUG -fPIC")

find_package(Threads REQUIRED)
find_library (UNWIND_LIBRARY NAMES unwind DOC "unwind library")
mark_as_advanced (UNWIND_LIBRARY)  ## Hides this variable from GUI.

if (NOT UNWIND_LIBRARY)
  Message(FATAL_ERROR  "libunwind8-dev is not installed but required for better glog stacktraces")
endif ()


function(add_third_party name)
  set(options SHARED)
  set(oneValueArgs CMAKE_PASS_FLAGS)
  set(multiValArgs BUILD_COMMAND INSTALL_COMMAND LIB)
  CMAKE_PARSE_ARGUMENTS(parsed "${options}" "${oneValueArgs}" "${multiValArgs}" ${ARGN})

  if (parsed_CMAKE_PASS_FLAGS)
    string(REPLACE " " ";" piped_CMAKE_ARGS ${parsed_CMAKE_PASS_FLAGS})
  endif()

  if (NOT parsed_INSTALL_COMMAND)
    set(parsed_INSTALL_COMMAND make install)
  endif()

  if (NOT parsed_BUILD_COMMAND)
    set(parsed_BUILD_COMMAND make -j4)
  endif()

  set(_DIR ${THIRD_PARTY_DIR}/${name})
  set(INSTALL_ROOT ${THIRD_PARTY_LIB_DIR}/${name})

  if (parsed_LIB)
    set(LIB_FILES "")

    foreach (_file ${parsed_LIB})
      LIST(APPEND LIB_FILES "${INSTALL_ROOT}/lib/${_file}")
      if (${_file} MATCHES ".*\.so$")
        set(LIB_TYPE SHARED)
      elseif (${_file} MATCHES ".*\.a$")
        set(LIB_TYPE STATIC)
      elseif("${_file}" STREQUAL "none")
        set(LIB_FILES "")
      else()
        MESSAGE(FATAL_ERROR "Unrecognized lib ${_file}")
      endif()
    endforeach(_file)
  else()
    set(LIB_PREFIX "${INSTALL_ROOT}/lib/lib${name}.")

    if(parsed_SHARED)
      set(LIB_TYPE SHARED)
      STRING(CONCAT LIB_FILES "${LIB_PREFIX}" "so")
    else()
      set(LIB_TYPE STATIC)
      STRING(CONCAT LIB_FILES "${LIB_PREFIX}" "a")
    endif(parsed_SHARED)
  endif()

  ExternalProject_Add(${name}_project
    DOWNLOAD_DIR ${_DIR}
    SOURCE_DIR ${_DIR}
    INSTALL_DIR ${INSTALL_ROOT}
    UPDATE_COMMAND ""

    BUILD_COMMAND ${parsed_BUILD_COMMAND}

    INSTALL_COMMAND ${parsed_INSTALL_COMMAND}

    # Wrap download, configure and build steps in a script to log output
    LOG_INSTALL ON
    LOG_DOWNLOAD ON
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_PATCH ON
    LOG_UPDATE ON

    CMAKE_GENERATOR "Unix Makefiles"
    BUILD_BYPRODUCTS ${LIB_FILES}
    # LIST_SEPARATOR | # Use the alternate list separator.
    # Can not use | because we use it inside sh/install_cmd

    # we need those CMAKE_ARGS for cmake based 3rd party projects.
    CMAKE_ARGS -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY:PATH=${INSTALL_ROOT}
        -DCMAKE_LIBRARY_OUTPUT_DIRECTORY:PATH=${INSTALL_ROOT}
        -DCMAKE_BUILD_TYPE:STRING=Release
        -DBUILD_TESTING=OFF
        "-DCMAKE_C_FLAGS:STRING=-O3" -DCMAKE_CXX_FLAGS=${THIRD_PARTY_CXX_FLAGS}
        -DCMAKE_INSTALL_PREFIX:PATH=${INSTALL_ROOT}
        ${piped_CMAKE_ARGS}
    ${parsed_UNPARSED_ARGUMENTS}
  )

  string(TOUPPER ${name} uname)
  file(MAKE_DIRECTORY ${INSTALL_ROOT}/include)

  set("${uname}_INCLUDE_DIR" "${INSTALL_ROOT}/include" PARENT_SCOPE)
  if (LIB_TYPE)
    set("${uname}_LIB_DIR" "${INSTALL_ROOT}/lib" PARENT_SCOPE)
    list(LENGTH LIB_FILES LIB_LEN)
    if (${LIB_LEN} GREATER 1)
      foreach (_file ${LIB_FILES})
        get_filename_component(base_name ${_file} NAME_WE)
        STRING(REGEX REPLACE "^lib" "" tname ${base_name})

        add_library(TRDP::${tname} ${LIB_TYPE} IMPORTED)
        add_dependencies(TRDP::${tname} ${name}_project)
        set_target_properties(TRDP::${tname} PROPERTIES IMPORTED_LOCATION ${_file}
                              INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_ROOT}/include)
      endforeach(_file)
    else()
        add_library(TRDP::${name} ${LIB_TYPE} IMPORTED)
        add_dependencies(TRDP::${name} ${name}_project)
        set_target_properties(TRDP::${name} PROPERTIES IMPORTED_LOCATION ${LIB_FILES}
                              INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_ROOT}/include)
    endif()
  endif()
endfunction()

# gflags
FetchContent_Declare(
  gflags
  URL https://github.com/gflags/gflags/archive/v2.2.2.zip
  SOURCE_DIR gflags
  PATCH_COMMAND patch -p1 < "${CMAKE_CURRENT_LIST_DIR}/../patches/gflags-v2.2.2.patch"
)

FetchContent_GetProperties(gflags)
if (NOT gflags_POPULATED)
    FetchContent_Populate(gflags)
    set(BUILD_gflags_nothreads_LIB OFF)
    set(BUILD_gflags_LIB ON)
    set(GFLAGS_INSTALL_STATIC_LIBS ON)

    add_subdirectory(${gflags_SOURCE_DIR} ${gflags_BINARY_DIR})
endif ()

FetchContent_Declare(
  glog
  GIT_REPOSITORY https://github.com/romange/glog.git
  GIT_TAG Prod
  PATCH_COMMAND patch -p1 < "${CMAKE_CURRENT_LIST_DIR}/../patches/glog-v0.4.0.patch"
  GIT_PROGRESS    TRUE
  GIT_SHALLOW     TRUE
)

FetchContent_GetProperties(glog)
if (NOT glog_POPULATED)
    FetchContent_Populate(glog)

    # We trick glog into compiling with gflags.
    set(HAVE_LIB_GFLAGS 1)
    set(WITH_GFLAGS OFF)
    add_subdirectory(${glog_SOURCE_DIR} ${glog_BINARY_DIR})

    set_property(TARGET glog APPEND PROPERTY
                 INCLUDE_DIRECTORIES $<TARGET_PROPERTY:gflags,INTERFACE_INCLUDE_DIRECTORIES>)
endif ()


FetchContent_Declare(
  gtest
  URL https://github.com/google/googletest/archive/release-1.10.0.zip
)

FetchContent_GetProperties(gtest)
if (NOT gtest_POPULATED)
    FetchContent_Populate(gtest)
    add_subdirectory(${gtest_SOURCE_DIR} ${gtest_BINARY_DIR})
endif ()

FetchContent_Declare(
  benchmark
  URL https://github.com/google/benchmark/archive/v1.5.1.zip
)

FetchContent_GetProperties(benchmark)
if (NOT benchmark_POPULATED)
    FetchContent_Populate(benchmark)
    set(BENCHMARK_ENABLE_TESTING OFF)
    set(BENCHMARK_ENABLE_INSTALL OFF)
    add_subdirectory(${benchmark_SOURCE_DIR} ${benchmark_BINARY_DIR})
endif ()



FetchContent_Declare(
    abseil_cpp
    GIT_REPOSITORY  https://github.com/abseil/abseil-cpp.git
    GIT_TAG         d0c433455801e1c1fb6f486f0b447e22f946ab52
    GIT_PROGRESS    TRUE
    GIT_SHALLOW     TRUE
)
FetchContent_GetProperties(abseil_cpp)
if(NOT abseil_cpp_POPULATED)
  FetchContent_Populate(abseil_cpp)

  add_subdirectory(${abseil_cpp_SOURCE_DIR} ${abseil_cpp_BINARY_DIR})
endif()


# set(Boost_DEBUG ON)
find_package(Boost 1.70.0 REQUIRED COMPONENTS fiber context system)
Message(STATUS "Found Boost ${Boost_LIBRARY_DIRS} ${Boost_LIB_VERSION} ${Boost_VERSION}")

add_definitions(-DBOOST_BEAST_SEPARATE_COMPILATION -DBOOST_ASIO_SEPARATE_COMPILATION)


add_third_party(
  gperf
  GIT_REPOSITORY https://github.com/gperftools/gperftools/
  GIT_TAG gperftools-2.8
  PATCH_COMMAND ./autogen.sh
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --enable-frame-pointers --enable-static=no
                    --enable-libunwind "CXXFLAGS=${THIRD_PARTY_CXX_FLAGS}"
                    --disable-deprecated-pprof --enable-aggressive-decommit-by-default
                    --prefix=${THIRD_PARTY_LIB_DIR}/gperf
  LIB libtcmalloc_and_profiler.so
)

add_library(fast_malloc SHARED IMPORTED)
add_dependencies(fast_malloc gperf_project)
set_target_properties(fast_malloc PROPERTIES IMPORTED_LOCATION
                      ${GPERF_LIB_DIR}/libtcmalloc_and_profiler.so
                      IMPORTED_LINK_INTERFACE_LIBRARIES unwind
                      INTERFACE_INCLUDE_DIRECTORIES ${GPERF_INCLUDE_DIR}
                      )

add_third_party(pmr
  GIT_REPOSITORY https://github.com/romange/pmr.git
)

add_third_party(
  xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.7.4
  SOURCE_SUBDIR cmake_unofficial
  CMAKE_PASS_FLAGS "-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF"
)
