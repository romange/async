// Copyright 2020, Beeri 15.  All rights reserved.
// Author: Roman Gershman (romange@gmail.com)
//
#pragma once
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/file_body.hpp>

#include "absl/strings/string_view.h"
// #include "util/asio_stream_adapter.h"

namespace util {
namespace http {

// URL consists of path and query delimited by '?'.
// query can be broken into query args delimited by '&'.
// Each query arg can be a pair of "key=value" values.
// In case there is not '=' delimiter, only the first field is filled.
using QueryParam = std::pair<absl::string_view, absl::string_view>;
typedef std::vector<QueryParam> QueryArgs;

typedef ::boost::beast::http::response<::boost::beast::http::string_body>
    StringResponse;

inline StringResponse MakeStringResponse(
    ::boost::beast::http::status st = ::boost::beast::http::status::ok) {
  return StringResponse(st, 11);
}

inline void SetMime(const char* mime, ::boost::beast::http::fields* dest) {
  dest->set(::boost::beast::http::field::content_type, mime);
}

inline absl::string_view as_absl(::boost::string_view s) {
  return absl::string_view(s.data(), s.size());
}

extern const char kHtmlMime[];
extern const char kJsonMime[];
extern const char kSvgMime[];
extern const char kTextMime[];
extern const char kXmlMime[];
extern const char kBinMime[];

QueryParam ParseQuery(absl::string_view str);
QueryArgs SplitQuery(absl::string_view query);
StringResponse ParseFlagz(const QueryArgs& args);

StringResponse BuildStatusPage(const QueryArgs& args, const char* resource_prefix);
StringResponse ProfilezHandler(const QueryArgs& args);

using FileResponse = ::boost::beast::http::response<::boost::beast::http::file_body>;
::boost::system::error_code LoadFileResponse(absl::string_view fname, FileResponse* resp);

}  // namespace http
}  // namespace util
