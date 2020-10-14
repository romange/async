
add_library(proactor_lib proactor_base.cc fiber_sched_algo.cc fiber_socket_base.cc)
cxx_link(proactor_lib base Boost::fiber)

add_subdirectory(fibers)
add_subdirectory(epoll)
add_subdirectory(uring)
add_subdirectory(html)
add_subdirectory(http)
