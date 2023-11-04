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

#include <map>
#include <string>
#include <vector>

#define TRITONJSON_STATUSTYPE TRITONSERVER_Error*
#define TRITONJSON_STATUSRETURN(M) \
  return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, (M).c_str())
#define TRITONJSON_STATUSSUCCESS nullptr
#include "triton/common/triton_json.h"

namespace triton::repoagent::dragonfly {

struct DragonflyConfig {
  std::string proxy;
  std::map<std::string, std::string> headers;
  std::vector<std::string> filter;

  explicit DragonflyConfig(triton::common::TritonJson::Value& config);
};

DragonflyConfig::DragonflyConfig(triton::common::TritonJson::Value& config)
{
  triton::common::TritonJson::Value proxy_json, header_json, filter_json;
  if (config.Find("proxy", &proxy_json)) {
    proxy_json.AsString(&proxy);
  }

  if (config.Find("header", &header_json)) {
    std::vector<std::string> header_keys;
    header_json.Members(&header_keys);
    for (const auto& key : header_keys) {
      std::string value;
      if (header_json.MemberAsString(key.c_str(), &value)) {
        headers[key] = value;
      }
    }
  }

  if (config.Find("filter", &filter_json)) {
    for (size_t i = 0; i < filter_json.ArraySize(); i++) {
      triton::common::TritonJson::Value value_json;
      std::string value;
      if (filter_json.At(i, &value_json) && value_json.AsString(&value)) {
        filter.push_back(value);
      }
    }
  }
}
}  // namespace triton::repoagent::dragonfly