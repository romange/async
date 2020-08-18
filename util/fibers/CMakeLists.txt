add_library(fibers_ext fibers_ext.cc fiberqueue_threadpool.cc)
cxx_link(fibers_ext base Boost::fiber absl_strings)

cxx_test(fibers_ext_test fibers_ext asio_fiber_lib LABELS CI)
