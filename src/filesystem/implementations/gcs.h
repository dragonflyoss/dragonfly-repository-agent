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

#include "common.h"
#include "common_utils.h"
#include "fstream"
#include "google/cloud/storage/client.h"
#include "iostream"
#include "set"
#include "sys/stat.h"
#include "triton/core/tritonserver.h"

namespace triton::repoagent::dragonfly {

namespace gcs = google::cloud::storage;

struct GCSCredential {
  std::string path_;

  explicit GCSCredential(triton::common::TritonJson::Value& cred_json);
};

GCSCredential::GCSCredential(triton::common::TritonJson::Value& cred_json)
{
  cred_json.AsString(&path_);
}

class GCSFileSystem : public FileSystem {
 public:
  // unify with S3/azure interface
  GCSFileSystem(const std::string& path, const GCSCredential& gs_cred);

  TRITONSERVER_Error* CheckClient();
  // unify with S3 interface
  TRITONSERVER_Error* CheckClient(const std::string& path)
  {
    return CheckClient();
  }

  TRITONSERVER_Error* LocalizePath(
      const std::string& location, const std::string& temp_dir,
      DragonflyConfig& config) override;

 private:
  TRITONSERVER_Error* FileExists(const std::string& path, bool* exists);
  TRITONSERVER_Error* IsDirectory(const std::string& path, bool* is_dir);
  TRITONSERVER_Error* GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents);
  static TRITONSERVER_Error* ParsePath(
      const std::string& path, std::string* bucket, std::string* object);
  std::string GenerateGetSignedUrl(
      std::string const& bucket_name, std::string const& object_name);

  std::unique_ptr<gcs::Client> client_;
};

GCSFileSystem::GCSFileSystem(
    const std::string& path, const GCSCredential& gs_cred)
{
  google::cloud::Options options;
  auto creds = gcs::oauth2::CreateServiceAccountCredentialsFromJsonFilePath(
      gs_cred.path_);
  if (creds) {
    options.set<gcs::Oauth2CredentialsOption>(*creds);
  } else {
    auto creds2 = gcs::oauth2::CreateComputeEngineCredentials();
    if (creds2->AuthorizationHeader()) {
      options.set<gcs::Oauth2CredentialsOption>(creds2);
    } else {
      options.set<gcs::Oauth2CredentialsOption>(
          gcs::oauth2::CreateAnonymousCredentials());
    }
  }
  client_ = std::make_unique<gcs::Client>(options);
}

TRITONSERVER_Error*
GCSFileSystem::CheckClient()
{
  if (!client_) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        "Unable to create GCS client. Check account credentials.");
  }
  return nullptr;
}

TRITONSERVER_Error*
GCSFileSystem::ParsePath(
    const std::string& path, std::string* bucket, std::string* object)
{
  // Get the bucket name and the object path. Return error if input is malformed
  std::string::size_type bucket_start = path.find("gs://");
  if (bucket_start != std::string::npos) {
    bucket_start += strlen("gs://");
  }
  std::string::size_type bucket_end = path.find('/', bucket_start);


  // If there isn't a second slash, the address has only the bucket
  if (bucket_end != std::string::npos && bucket_end > bucket_start) {
    *bucket = path.substr(bucket_start, bucket_end - bucket_start);
    *object = path.substr(bucket_end + 1);
  } else {
    *bucket = path.substr(bucket_start);
    *object = "";
  }

  if (bucket->empty()) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL, "No bucket name found in path");
  }

  return nullptr;
}

std::string
GCSFileSystem::GenerateGetSignedUrl(
    std::string const& bucket_name, std::string const& object_name)
{
  using ::google::cloud::StatusOr;
  StatusOr<std::string> signed_url = client_->CreateV4SignedUrl(
      "GET", bucket_name, object_name,
      gcs::SignedUrlDuration(std::chrono::minutes(150)));

  if (!signed_url)
    throw std::move(signed_url).status();

  return signed_url.value();
}

TRITONSERVER_Error*
GCSFileSystem::FileExists(const std::string& path, bool* exists)
{
  *exists = false;

  std::string bucket, object;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

  // Make a request for metadata and check the response
  google::cloud::StatusOr<gcs::ObjectMetadata> object_metadata =
      client_->GetObjectMetadata(bucket, object);

  if (object_metadata) {
    *exists = true;
    return nullptr;
  }

  // GCS doesn't make objects for directories, so it could still be a directory
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  *exists = is_dir;

  return nullptr;
}

TRITONSERVER_Error*
GCSFileSystem::IsDirectory(const std::string& path, bool* is_dir)
{
  *is_dir = false;
  std::string bucket, object_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &object_path));

  google::cloud::StatusOr<gcs::BucketMetadata> bucket_metadata =
      client_->GetBucketMetadata(bucket);

  if (!bucket_metadata) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Could not get MetaData for bucket with name " + bucket + " : " +
         bucket_metadata.status().message())
            .c_str());
  }

  if (object_path.empty()) {
    *is_dir = true;
    return nullptr;
  }

  for (auto&& object_metadata :
       client_->ListObjects(bucket, gcs::Prefix(AppendSlash(object_path)))) {
    if (object_metadata) {
      *is_dir = true;
      break;
    }
  }
  return nullptr;
}

TRITONSERVER_Error*
GCSFileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
{
  std::string bucket, dir_path;
  RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
  // Append a slash to make it easier to list contents
  std::string full_dir = AppendSlash(dir_path);

  // Get objects with prefix equal to full directory path
  for (auto&& object_metadata :
       client_->ListObjects(bucket, gcs::Prefix(full_dir))) {
    if (!object_metadata) {
      std::string msg = "Could not list contents of directory at " + path +
                        " : " + object_metadata.status().message();
      return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL, msg.c_str());
    }

    // In the case of empty directories, the directory itself will appear here
    if (object_metadata->name() == full_dir) {
      continue;
    }

    // We have to make sure that subdirectory contents do not appear here
    std::string name = object_metadata->name();
    std::string::size_type item_start = name.find(full_dir) + full_dir.size();
    // GCS response prepends parent directory name
    std::string::size_type item_end = name.find('/', item_start);

    // Let set take care of subdirectory contents
    std::string item;
    if (item_end !=
        std::string::npos) {  // ensure that item_end is valid before substr
      item = name.substr(item_start, item_end - item_start);
    } else {
      item = name.substr(item_start);
    }
    contents->insert(item);

    // Fail-safe check to ensure the item name is not empty
    if (item.empty()) {
      return TRITONSERVER_ErrorNew(
          TRITONSERVER_ERROR_INTERNAL,
          ("Cannot handle item with empty name at " + path).c_str());
    }
  }
  return nullptr;
}

TRITONSERVER_Error*
GCSFileSystem::LocalizePath(
    const std::string& location, const std::string& temp_dir,
    DragonflyConfig& config)
{
  bool exists;
  //
  RETURN_IF_ERROR(FileExists(location, &exists));
  if (!exists) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL, "File does not exist");
  }

  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(temp_dir, &is_dir));
  if (!is_dir) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "GCS file localization not yet implemented");
  }

  std::set<std::string> contents, filenames;
  RETURN_IF_ERROR(GetDirectoryContents(location, &filenames));
  for (auto itr = filenames.begin(); itr != filenames.end(); ++itr) {
    contents.insert(JoinPath({location, *itr}));
  }

  while (!contents.empty()) {
    std::set<std::string> tmp_contents = contents;
    contents.clear();
    for (const auto& gcs_fpath : tmp_contents) {
      bool is_subdir;
      std::string gcs_removed_path = gcs_fpath.substr(location.size());
      std::string local_fpath = JoinPath({temp_dir, gcs_removed_path});
      RETURN_IF_ERROR(IsDirectory(gcs_fpath, &is_subdir));
      if (is_subdir) {
#ifdef _WIN32
        int status = mkdir(const_cast<char*>(local_fpath.c_str()));
#else
        int status = mkdir(
            const_cast<char*>(local_fpath.c_str()),
            S_IRUSR | S_IWUSR | S_IXUSR);
#endif
        if (status == -1) {
          return TRITONSERVER_ErrorNew(
              TRITONSERVER_ERROR_INTERNAL,
              ("Failed to create local folder: " + local_fpath +
               ", errno:" + strerror(errno))
                  .c_str());
        }

        std::set<std::string> subdir_contents;
        RETURN_IF_ERROR(GetDirectoryContents(gcs_fpath, &subdir_contents));
        for (auto itr = subdir_contents.begin(); itr != subdir_contents.end();
             ++itr) {
          contents.insert(JoinPath({gcs_fpath, *itr}));
        }
      } else {
        std::string file_bucket, file_object;
        RETURN_IF_ERROR(ParsePath(gcs_fpath, &file_bucket, &file_object));

        std::string signed_url = GenerateGetSignedUrl(file_bucket, file_object);
        RETURN_IF_ERROR(DownloadFile(signed_url, local_fpath, config));
      }
    }
  }

  return nullptr;
}
}  // namespace triton::repoagent::dragonfly
