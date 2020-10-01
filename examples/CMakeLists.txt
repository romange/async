add_subdirectory(pingserver)

add_executable(echo_server echo_server.cc)
cxx_link(echo_server base uring_fiber_lib)
