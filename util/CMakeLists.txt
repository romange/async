
add_library(proactor_lib accept_server.cc fiber_sched_algo.cc fiber_socket_base.cc
            listener_interface.cc proactor_base.cc proactor_pool.cc)
cxx_link(proactor_lib base absl::flat_hash_map Boost::fiber)

cxx_test(accept_server_test uring_fiber_lib)

add_subdirectory(fibers)
add_subdirectory(epoll)
add_subdirectory(uring)
add_subdirectory(html)
add_subdirectory(http)

