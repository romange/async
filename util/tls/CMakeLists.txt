add_library(tls_lib ssl_stream.cc)
cxx_link(tls_lib base ssl crypto)

cxx_test(ssl_stream_test tls_lib LABELS CI)
