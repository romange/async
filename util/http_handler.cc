// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#include "util/http_handler.h"

#include <boost/beast/core.hpp>  // for flat_buffer.
#include <boost/beast/http.hpp>

#include "base/logging.h"

namespace util {
using namespace http;
using namespace std;
using namespace boost;
namespace h2 = beast::http;

namespace {

void FilezHandler(const QueryArgs& args, HttpContext* send) {
  absl::string_view file_name;
  for (const auto& k_v : args) {
    if (k_v.first == "file") {
      file_name = k_v.second;
    }
  }
  if (file_name.empty()) {
    http::StringResponse resp = MakeStringResponse(h2::status::unauthorized);
    return send->Invoke(std::move(resp));
  }

  FileResponse fresp;
  string fname(file_name);
  auto ec = LoadFileResponse(fname, &fresp);
  if (ec) {
    StringResponse res = MakeStringResponse(h2::status::not_found);
    SetMime(kTextMime, &res);
    if (ec == boost::system::errc::no_such_file_or_directory)
      res.body() = "The resource '" + fname + "' was not found.";
    else
      res.body() = "Error '" + ec.message() + "'.";
    return send->Invoke(std::move(res));
  }

  return send->Invoke(std::move(fresp));
}

}  // namespace

HttpListenerBase::HttpListenerBase() {
  favicon_ =
      "https://rawcdn.githack.com/romange/gaia/master/util/http/"
      "favicon-32x32.png";
  resource_prefix_ = "https://cdn.jsdelivr.net/gh/romange/gaia/util/http";
}

bool HttpListenerBase::HandleRoot(const RequestType& request,
                                  HttpContext* cntx) const {
  absl::string_view target = as_absl(request.target());
  if (target == "/favicon.ico") {
    h2::response<h2::string_body> resp =
        MakeStringResponse(h2::status::moved_permanently);
    resp.set(h2::field::location, favicon_);
    resp.set(h2::field::server, "GAIA");
    resp.keep_alive(request.keep_alive());

    cntx->Invoke(std::move(resp));
    return true;
  }

  absl::string_view path, query;
  tie(path, query) = ParseQuery(target);
  auto args = SplitQuery(query);

  if (path == "/") {
    cntx->Invoke(BuildStatusPage(args, resource_prefix_));
    return true;
  }

  if (path == "/flagz") {
    h2::response<h2::string_body> resp(h2::status::ok, request.version());
    cntx->Invoke(ParseFlagz(args));
    return true;
  }

  if (path == "/filez") {
    FilezHandler(args, cntx);
    return true;
  }

  if (path == "/profilez") {
    cntx->Invoke(ProfilezHandler(args));
    return true;
  }
  return false;
}

bool HttpListenerBase::RegisterCb(absl::string_view path, RequestCb cb) {
  CbInfo cb_info{.cb = cb};

  auto res = cb_map_.emplace(path, cb_info);
  return res.second;
}

HttpHandler2::HttpHandler2(const HttpListenerBase* base) : base_(base) {
}

void HttpHandler2::HandleRequests() {
  CHECK(socket_->IsOpen());
  beast::flat_buffer buffer;
  RequestType request;

  system::error_code ec;
  AsioStreamAdapter<> asa(*socket_);

  while (true) {
    h2::read(asa, buffer, request, ec);
    if (ec) {
      break;
    }
    HttpContext cntx(asa);
    VLOG(1) << "Full Url: " << request.target();
    HandleOne(request, &cntx);
  }
  VLOG(1) << "HttpHandler2 exit";
}

void HttpHandler2::HandleOne(const RequestType& req, HttpContext* cntx) {
  CHECK(base_);

  if (base_->HandleRoot(req, cntx)) {
    return;
  }
  absl::string_view target = as_absl(req.target());
  absl::string_view path, query;
  tie(path, query) = ParseQuery(target);
  VLOG(2) << "Searching for " << path;

  auto it = base_->cb_map_.find(path);
  if (it == base_->cb_map_.end()) {
    h2::response<h2::string_body> resp(h2::status::unauthorized, req.version());
    return cntx->Invoke(std::move(resp));
  }
  auto args = SplitQuery(query);
  it->second.cb(args, cntx);
}

}  // namespace util
