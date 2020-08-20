add_library(base arena.cc init.cc logging.cc proc_util.cc  pthread_utils.cc)
cxx_link(base glog::glog gflags::gflags atomic rt
         absl::symbolize absl::failure_signal_handler)  # rt for timer_create etc.

# Define default gtest_main for tests.
add_library(gtest_main_ext gtest_main.cc)
target_link_libraries(gtest_main_ext gmock base benchmark TRDP::gperf)

cxx_test(mpmc_bounded_queue_test base LABELS CI)
cxx_test(arena_test base LABELS CI)


