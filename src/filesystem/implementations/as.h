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

#include "azure/storage/blobs.hpp"
#include "azure/storage/common/storage_credential.hpp"
#include "vector"

#include "common.h"
#include "common_utils.h"

#undef LOG_INFO
#undef LOG_WARNING

namespace triton::repoagent::dragonfly {

namespace as = Azure::Storage;
namespace asb = Azure::Storage::Blobs;
const std::string AS_URL_PATTERN = "as://([^/]+)/([^/?]+)(?:/([^?]*))?(\\?.*)?";

struct ASCredential {
  std::string account_str_;
  std::string account_key_;

  explicit ASCredential(triton::common::TritonJson::Value& cred_json);
};


ASCredential::ASCredential(triton::common::TritonJson::Value& cred_json)
{
  triton::common::TritonJson::Value account_str_json, account_key_json;
  if (cred_json.Find("account_str", &account_str_json))
    account_str_json.AsString(&account_str_);
  if (cred_json.Find("account_key", &account_key_json))
    account_key_json.AsString(&account_key_);
}

class ASFileSystem : public FileSystem {
 public:
  ASFileSystem(const std::string& path, const ASCredential& as_cred);

  TRITONSERVER_Error* CheckClient(const std::string& path);

  TRITONSERVER_Error* LocalizePath(
      const std::string& location, const std::string& temp_dir, DragonflyConfig& config_path) override;


  TRITONSERVER_Error* FileExists(const std::string& path, bool* exists);
  TRITONSERVER_Error* IsDirectory(const std::string& path, bool* is_dir);

 private:
  TRITONSERVER_Error* ParsePath(
      const std::string& path, std::string* container, std::string* blob);

  // 'callback' will be invoked when directory content is received, it may
  // be invoked multiple times within the same ListDirectory() call if the
  // result is paged.
  TRITONSERVER_Error* ListDirectory(
      const std::string& container, const std::string& dir_path,
      const std::function<TRITONSERVER_Error*(
          const std::vector<asb::Models::BlobItem>& blobs,
          const std::vector<std::string>& blob_prefixes)>&
          callback);

  TRITONSERVER_Error* DownloadFolder(
      const std::string& container, const std::string& path,
      const std::string& dest, DragonflyConfig& config);

  std::shared_ptr<asb::BlobServiceClient> client_;
  re2::RE2 as_regex_;
};

TRITONSERVER_Error*
ASFileSystem::ParsePath(
    const std::string& path, std::string* container, std::string* blob)
{
  std::string host_name, query;
  if (!RE2::FullMatch(path, as_regex_, &host_name, container, blob, &query)) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL, ("Invalid azure storage path: " + path).c_str());
  }
  return nullptr;
}

ASFileSystem::ASFileSystem(const std::string& path, const ASCredential& as_cred)
    : as_regex_(AS_URL_PATTERN)
{
  std::string host_name, container, blob_path, query;
  if (RE2::FullMatch(
          path, as_regex_, &host_name, &container, &blob_path, &query)) {
    size_t pos = host_name.rfind(".blob.core.windows.net");
    std::string account_name;
    if (as_cred.account_str_.empty()) {
      if (pos != std::string::npos) {
        account_name = host_name.substr(0, pos);
      } else {
        account_name = host_name;
      }
    } else {
      account_name = as_cred.account_str_;
    }
    std::string service_url(
        "https://" + account_name + ".blob.core.windows.net");

    if (!as_cred.account_key_.empty()) {
      // Shared Key
      auto cred = std::make_shared<as::StorageSharedKeyCredential>(
          account_name, as_cred.account_key_);
      client_ = std::make_shared<asb::BlobServiceClient>(service_url, cred);
    } else {
      client_ = std::make_shared<asb::BlobServiceClient>(service_url);
    }
  }
}

TRITONSERVER_Error*
ASFileSystem::CheckClient(const std::string& path)
{
  if (client_ == nullptr) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        "Unable to create Azure filesystem client. Check account credentials.");
  }
  return nullptr;
}

TRITONSERVER_Error*
ASFileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;
  std::string container, blob_path;
  RETURN_IF_ERROR(ParsePath(path, &container, &blob_path));

  auto container_client = client_->GetBlobContainerClient(container);
  auto options = asb::ListBlobsOptions();
  // Append a slash to make it easier to list contents
  std::string full_dir = AppendSlash(blob_path);
  options.Prefix = full_dir;
  try {
    for (auto blobPage = container_client.ListBlobsByHierarchy("/", options);
         blobPage.HasPage(); blobPage.MoveToNextPage()) {
      if ((blobPage.Blobs.size() == 1) &&
          (blobPage.Blobs[0].Name == blob_path)) {
        // It's a file
        return nullptr;
      }
      *is_dir =
          ((blobPage.Blobs.size() > 0) || (blobPage.BlobPrefixes.size() > 0));
      break;
    }
  }
  catch (as::StorageException& ex) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to check if directory at " + path + ":" + ex.what()).c_str());
  }

  return nullptr;
};


TRITONSERVER_Error*
ASFileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = false;

  std::string container, blob;
  RETURN_IF_ERROR(ParsePath(path, &container, &blob));

  auto container_client = client_->GetBlobContainerClient(container);
  auto options = asb::ListBlobsOptions();
  options.Prefix = blob;
  try {
    for (auto blobPage = container_client.ListBlobsByHierarchy("/", options);
         blobPage.HasPage(); blobPage.MoveToNextPage()) {
      // If any entries are returned from ListBlobs, the file / directory exists
      *exists =
          ((blobPage.Blobs.size() > 0) || (blobPage.BlobPrefixes.size() > 0));
      break;
    }
  }
  catch (as::StorageException& ex) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to check if file exists at " + path + ":" + ex.what()).c_str());
  }

  return nullptr;
}

TRITONSERVER_Error*
ASFileSystem::DownloadFolder(
    const std::string& container, const std::string& path,
    const std::string& dest, DragonflyConfig& config)
{
  auto container_client = client_->GetBlobContainerClient(container);
  auto func = [&](const std::vector<asb::Models::BlobItem>& blobs,
                  const std::vector<std::string>& blob_prefixes) -> TRITONSERVER_Error* {
    for (const auto& blob_item : blobs) {
      const auto& local_path = JoinPath({dest, BaseName(blob_item.Name)});
      try {
        std::string url = container_client.GetBlobClient(blob_item.Name).GetUrl();
        RETURN_IF_ERROR(DownloadFile(url, local_path, config));
      }
      catch (as::StorageException& ex) {
        return TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            ("Failed to download file at " + blob_item.Name + ":" + ex.what()).c_str());
      }
    }
    for (const auto& directory_item : blob_prefixes) {
      const auto& local_path = JoinPath({dest, BaseName(directory_item)});
      int status = mkdir(
          const_cast<char*>(local_path.c_str()), S_IRUSR | S_IWUSR | S_IXUSR);
      if (status == -1) {
        return TRITONSERVER_ErrorNew(
            TRITONSERVER_ERROR_INTERNAL,
            ("Failed to create local folder: " + local_path +
             ", errno:" + strerror(errno)).c_str());
      }
      RETURN_IF_ERROR(DownloadFolder(container, directory_item, local_path, config));
    }
    return nullptr;
  };
  return ListDirectory(container, path, func);
}

TRITONSERVER_Error*
ASFileSystem::LocalizePath(
    const std::string& location, const std::string& temp_dir, DragonflyConfig& config)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(location, &exists));
  if (!exists) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("directory or file does not exist at " + location).c_str());
  }

  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(location, &is_dir));
  if (!is_dir) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        ("AS file localization not yet implemented " + location).c_str());
  }

  std::string container, blob;
  RETURN_IF_ERROR(ParsePath(location, &container, &blob));
  return DownloadFolder(container, blob, temp_dir, config);
}

    TRITONSERVER_Error*
ASFileSystem::ListDirectory(
    const std::string& container, const std::string& dir_path,
    const std::function<TRITONSERVER_Error*(
        const std::vector<asb::Models::BlobItem>& blobs,
        const std::vector<std::string>& blob_prefixes)>&
        callback)
{
  auto container_client = client_->GetBlobContainerClient(container);
  auto options = asb::ListBlobsOptions();
  // Append a slash to make it easier to list contents
  std::string full_dir = AppendSlash(dir_path);
  options.Prefix = full_dir;

  try {
    for (auto blobPage = container_client.ListBlobsByHierarchy("/", options);
         blobPage.HasPage(); blobPage.MoveToNextPage()) {
      // per-page per-blob
      RETURN_IF_ERROR(callback(blobPage.Blobs, blobPage.BlobPrefixes));
    }
  }
  catch (as::StorageException& ex) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to get contents of directory " + dir_path + ":" + ex.what()).c_str());
  }

  return nullptr;
}

}  // namespace triton::repoagent::dragonfly
