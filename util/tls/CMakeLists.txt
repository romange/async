add_library(tls_lib tls_engine.cc tls_socket.cc)
cxx_link(tls_lib proactor_lib ssl crypto)

cxx_test(tls_engine_test tls_lib Boost::fiber LABELS CI)
