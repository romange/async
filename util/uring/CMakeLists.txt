add_library(uring_fiber_lib fiber_socket.cc
            # we need prebuilt_asio for errrors support, consider using our own errors
            proactor.cc uring_pool.cc uring_fiber_algo.cc)
cxx_link(uring_fiber_lib http_common base proactor_lib TRDP::uring)

cxx_test(proactor_test uring_fiber_lib)
