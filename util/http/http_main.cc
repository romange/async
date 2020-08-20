// Copyright 2013, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/http/http_common.h"
#include "util/uring/accept_server.h"
#include "util/uring/proactor_pool.h"

// #include "util/http/http_conn_handler.h"
#include "absl/strings/str_join.h"
#include "base/init.h"
#include "util/html/sorted_table.h"
#include "util/uring/http_handler.h"
#include "util/uring/varz.h"

DEFINE_int32(port, 8080, "Port number.");

using namespace std;
using namespace boost;
using namespace util;
namespace h2 = beast::http;

uring::VarzQps http_qps("bar-qps");

int main(int argc, char** argv) {
  MainInitGuard guard(&argc, &argv);

  uring::ProactorPool pool;
  pool.Run();

  uring::AcceptServer server(&pool);

  uring::HttpListener<>* listener = new uring::HttpListener<>;
  auto cb = [](const http::QueryArgs& args, uring::HttpContext* send) {
    http::StringResponse resp = http::MakeStringResponse(h2::status::ok);
    resp.body() = "Bar";

    http::SetMime(http::kTextMime, &resp);
    resp.set(beast::http::field::server, "uring");
    http_qps.Inc();

    return send->Invoke(std::move(resp));
  };

  listener->RegisterCb("/foo", cb);

  auto table_cb = [](const http::QueryArgs& args, uring::HttpContext* send) {
    using html::SortedTable;

    auto cell = [](auto i, auto j) { return absl::StrCat("Val", i, "_", j); };

    http::StringResponse resp = http::MakeStringResponse(h2::status::ok);
    resp.body() = SortedTable::HtmlStart();
    SortedTable::StartTable({"Col1", "Col2", "Col3", "Col4"}, &resp.body());
    for (size_t i = 0; i < 300; ++i) {
      SortedTable::Row({cell(1, i), cell(2, i), cell(3, i), cell(4, i)}, &resp.body());
    }
    SortedTable::EndTable(&resp.body());
    return send->Invoke(std::move(resp));
  };
  listener->RegisterCb("/table", table_cb);

  http_qps.Init(&pool);

  uint16_t port = server.AddListener(FLAGS_port, listener);
  LOG(INFO) << "Listening on port " << port;

  server.Run();
  server.Wait();
  pool.Stop();

  LOG(INFO) << "Exiting server...";
  return 0;
}
