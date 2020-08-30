add_library(epoll_fiber_lib ev_controller.cc epoll_fiber_scheduler.cc)
cxx_link(epoll_fiber_lib http_common base absl::flat_hash_map Boost::fiber)

