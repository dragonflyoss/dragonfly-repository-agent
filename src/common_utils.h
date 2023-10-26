/*
*     Copyright 2023 The Dragonfly Authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
 */
#pragma once


#include <fstream>
#include <sstream>
#include <iostream>

#include "config.h"
#include "triton/core/tritonserver.h"
#include "curl/curl.h"

namespace triton::repoagent::dragonfly {

// Helper function to take care of lack of trailing slashes
std::string
AppendSlash(const std::string& name)
{
  if (name.empty() || (name.back() == '/')) {
    return name;
  }
  return (name + "/");
}

bool
IsAbsolutePath(const std::string& path)
{
  return !path.empty() && (path[0] == '/');
}

std::string
JoinPath(std::initializer_list<std::string> segments)
{
  std::string joined;

  for (const auto& seg : segments) {
    if (joined.empty()) {
      joined = seg;
    } else if (IsAbsolutePath(seg)) {
      if (joined[joined.size() - 1] == '/') {
        joined.append(seg.substr(1));
      } else {
        joined.append(seg);
      }
    } else {  // !IsAbsolutePath(seg)
      if (joined[joined.size() - 1] != '/') {
        joined.append("/");
      }
      joined.append(seg);
    }
  }

  return joined;
}

std::string
BaseName(const std::string& path)
{
  if (path.empty()) {
    return path;
  }

  size_t last = path.size() - 1;
  while ((last > 0) && (path[last] == '/')) {
    last -= 1;
  }

  if (path[last] == '/') {
    return {};
  }

  const size_t idx = path.find_last_of('/', last);
  if (idx == std::string::npos) {
    return path.substr(0, last + 1);
  }

  return path.substr(idx + 1, last - idx);
}

TRITONSERVER_Error*
DownloadFile (const std::string &url, const std::string &path, DragonflyConfig& config) {
  CURL* curl;

  curl = curl_easy_init();
  if (!curl) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        "Failed to initialize CURL.");
  }

  FILE* fp = fopen(path.c_str(), "wb");
  if (!fp) {
    curl_easy_cleanup(curl);
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to open file at path: " + path).c_str());
  }


  struct curl_slist *headers = NULL;

  auto cleanup = [&]() {
    if (fp) fclose(fp);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  };

  auto write_data = [](void* ptr, size_t size, size_t nmemb, void* stream) -> size_t {
    size_t written = fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
    return written;
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

  for(const auto& header : config.headers) {
    std::string header_str = header.first + ": " + header.second;
    headers = curl_slist_append(headers, header_str.c_str());
    if (!headers) {
      cleanup();
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          "Failed to append headers.");
    }
  }

  if (!config.filter.empty()) {
    std::ostringstream oss;
    for (size_t i = 0; i < config.filter.size(); ++i) {
      if (i != 0) oss << "&";
      oss << config.filter[i];
    }
    headers = curl_slist_append(headers, ("X-Dragonfly-Filter: " + oss.str()).c_str());
    if (!headers) {
      cleanup();
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          "Failed to append filters.");
    }
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  if (!config.proxy.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, config.proxy.c_str());
  }

  CURLcode res = curl_easy_perform(curl);

  if(res != CURLE_OK) {
    cleanup();
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        curl_easy_strerror(res));
  }

  cleanup();
  return nullptr;
}

TRITONSERVER_Error*
ReadLocalFile(const std::string &path, std::string *contents) {
  if (!contents) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        "Output string pointer is null.");
  }

  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to open text file for read " + path).c_str());
  }

  std::ostringstream oss;
  oss << in.rdbuf();
  *contents = oss.str();

  if (in.bad()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Error reading from text file " + path).c_str());
  }

  return nullptr;
}

}
