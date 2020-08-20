// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//

#include "util/http/http_common.h"

#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "base/flags.h"
#include "base/logging.h"

namespace util {
namespace http {
using namespace std;
using namespace boost;

namespace h2 = beast::http;

namespace {

void HandleVModule(absl::string_view str) {
  vector<absl::string_view> parts = absl::StrSplit(str, ",", absl::SkipEmpty());
  for (absl::string_view p : parts) {
    size_t sep = p.find('=');
    int32_t level = 0;
    if (sep != absl::string_view::npos &&
        absl::SimpleAtoi(p.substr(sep + 1), &level)) {
      string module_expr = string(p.substr(0, sep));
      int prev = google::SetVLOGLevel(module_expr.c_str(), level);
      LOG(INFO) << "Setting module " << module_expr << " to loglevel " << level
                << ", prev: " << prev;
    }
  }
}

}  // namespace

const char kHtmlMime[] = "text/html";
const char kJsonMime[] = "application/json";
const char kSvgMime[] = "image/svg+xml";
const char kTextMime[] = "text/plain";
const char kXmlMime[] = "application/xml";
const char kBinMime[] = "application/octet-stream";;

QueryParam ParseQuery(absl::string_view str) {
  std::pair<absl::string_view, absl::string_view> res;
  size_t pos = str.find('?');
  res.first = str.substr(0, pos);
  if (pos != absl::string_view::npos) {
    res.second = str.substr(pos + 1);
  }
  return res;
}

QueryArgs SplitQuery(absl::string_view query) {
  vector<absl::string_view> args = absl::StrSplit(query, '&');
  vector<std::pair<absl::string_view, absl::string_view>> res(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    size_t pos = args[i].find('=');
    res[i].first = args[i].substr(0, pos);
    res[i].second =
        (pos == absl::string_view::npos) ? absl::string_view() : args[i].substr(pos + 1);
  }
  return res;
}

h2::response<h2::string_body> ParseFlagz(const QueryArgs& args) {
  h2::response<h2::string_body> response(h2::status::ok, 11);

  absl::string_view flag_name;
  absl::string_view value;
  for (const auto& k_v : args) {
    if (k_v.first == "flag") {
      flag_name = k_v.second;
    } else if (k_v.first == "value") {
      value = k_v.second;
    }
  }
  if (!flag_name.empty()) {
    gflags::CommandLineFlagInfo flag_info;
    string fname(flag_name);
    if (!gflags::GetCommandLineFlagInfo(fname.c_str(), &flag_info)) {
      response.body() = "Flag not found \n";
    } else {
      SetMime(kHtmlMime, &response);
      response.body()
          .append("<p>Current value ")
          .append(flag_info.current_value)
          .append("</p>");
      string new_val(value);
      string res = gflags::SetCommandLineOption(fname.c_str(), new_val.c_str());
      response.body().append("Flag ").append(res);

      if (flag_name == "vmodule") {
        HandleVModule(value);
      }
    }
  } else if (args.size() == 1) {
    LOG(INFO) << "Printing all flags";
    std::vector<gflags::CommandLineFlagInfo> flags;
    gflags::GetAllFlags(&flags);
    for (const auto& v : flags) {
      response.body()
          .append("--")
          .append(v.name)
          .append(": ")
          .append(v.current_value)
          .append("\n");
      SetMime(kTextMime, &response);
    }
  }
  return response;
}

::boost::system::error_code LoadFileResponse(absl::string_view fname,
                                             FileResponse* resp) {
  FileResponse::body_type::value_type body;
  system::error_code ec;
  body.open(fname.data(), boost::beast::file_mode::scan, ec);
  if (ec) {
    return ec;
  }

  size_t sz = body.size();
  *resp =
      FileResponse{std::piecewise_construct, std::make_tuple(std::move(body)),
                   std::make_tuple(h2::status::ok, 11)};

  const char* mime = kHtmlMime;
  if (absl::EndsWith(fname, ".svg")) {
    mime = kSvgMime;
  } else if (absl::EndsWith(fname, ".html")) {
    mime = kHtmlMime;
  } else {
    mime = kTextMime;
  }
  SetMime(mime, resp);
  resp->content_length(sz);
  resp->swap(*resp);

  return ec;
}

}  // namespace http
}  // namespace util
