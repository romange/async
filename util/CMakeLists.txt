
add_library(proactor_lib accept_server.cc fiber_sched_algo.cc fiber_socket_base.cc
            http_handler.cc listener_interface.cc prebuilt_asio.cc proactor_base.cc proactor_pool.cc
            sliding_counter.cc varz.cc)
cxx_link(proactor_lib base http_common absl::flat_hash_map Boost::fiber)

cxx_test(accept_server_test uring_fiber_lib)

add_subdirectory(fibers)
add_subdirectory(epoll)
add_subdirectory(uring)
add_subdirectory(html)
add_subdirectory(http)

