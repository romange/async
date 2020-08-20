add_library(uring_fiber_lib accept_server.cc fiber_socket.cc
            # we need prebuilt_asio for errrors support, consider using our own errors
            prebuilt_asio.cc proactor.cc proactor_pool.cc sliding_counter.cc
            uring_fiber_algo.cc varz_node.cc varz.cc)
cxx_link(uring_fiber_lib base absl::flat_hash_map Boost::fiber TRDP::uring)

cxx_test(proactor_test uring_fiber_lib)
#cxx_test(accept_server_test uring_fiber_lib http_beast_prebuilt)
