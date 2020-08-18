add_library(base init.cc logging.cc)
cxx_link(base glog::glog gflags::gflags atomic rt
         absl::symbolize absl::failure_signal_handler)  # rt for timer_create etc.

# Define default gtest_main for tests.
add_library(gtest_main_ext gtest_main.cc)
target_link_libraries(gtest_main_ext gmock base benchmark)

cxx_test(mpmc_bounded_queue_test base LABELS CI)



