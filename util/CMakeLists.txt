
add_library(proactor_lib fiber_sched_algo.cc fiber_socket_base.cc listener_interface.cc
            proactor_base.cc proactor_pool.cc)
cxx_link(proactor_lib base absl::flat_hash_map Boost::fiber)

add_subdirectory(fibers)
add_subdirectory(epoll)
add_subdirectory(uring)
add_subdirectory(html)
add_subdirectory(http)
