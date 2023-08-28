//
//      Copyright 2023 The Dragonfly Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// Dragonfly Repository Agent that implements the TRITONREPOAGENT API.
//
#include <algorithm>
#include <iostream>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <set>

#include "triton/core/tritonrepoagent.h"
#include "triton/core/tritonserver.h"
#include "triton/common/triton_json.h"
#include "aws/core/Aws.h"
#include "aws/s3/S3Client.h"
#include "httplib.h"
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/ListObjectsV2Result.h>

const uint64_t expiresInSec = 3600;

namespace triton { namespace repoagent { namespace dragonfly {

namespace {
//
// ErrorException
//
// Exception thrown if error occurs while running DragonflyRepoAgent
//
struct ErrorException {
    ErrorException(TRITONSERVER_Error *err) : err_(err) {}

    TRITONSERVER_Error *err_;
};

#define THROW_IF_TRITON_ERROR(X)                                    \
  do {                                                              \
    TRITONSERVER_Error* tie_err__ = (X);                            \
    if (tie_err__ != nullptr) {                                     \
      throw triton::repoagent::checksum::ErrorException(tie_err__); \
    }                                                               \
  } while (false)


#define THROW_TRITON_ERROR(CODE, MSG)                                 \
  do {                                                                \
    TRITONSERVER_Error* tie_err__ = TRITONSERVER_ErrorNew(CODE, MSG); \
    throw triton::repoagent::checksum::ErrorException(tie_err__);     \
  } while (false)


#define RETURN_IF_ERROR(X)               \
  do {                                   \
    TRITONSERVER_Error* rie_err__ = (X); \
    if (rie_err__ != nullptr) {          \
      return rie_err__;                  \
    }                                    \
  } while (false)

struct S3Credential {
    std::string secret_key_;
    std::string key_id_;
    std::string region_;
    std::string session_token_;
    std::string profile_name_;

    S3Credential();
    S3Credential(triton::common::TritonJson::Value& cred_json);
};

S3Credential::S3Credential()
{
    const auto to_str = [](const char* s) -> std::string {
        return (s != nullptr ? std::string(s) : "");
    };
    secret_key_ = to_str(std::getenv("AWS_SECRET_ACCESS_KEY"));
    key_id_ = to_str(std::getenv("AWS_ACCESS_KEY_ID"));
    region_ = to_str(std::getenv("AWS_DEFAULT_REGION"));
    session_token_ = to_str(std::getenv("AWS_SESSION_TOKEN"));
    profile_name_ = to_str(std::getenv("AWS_PROFILE"));
}

S3Credential::S3Credential(triton::common::TritonJson::Value& cred_json)
{
    triton::common::TritonJson::Value secret_key_json, key_id_json, region_json,
    session_token_json, profile_json;
    if (cred_json.Find("secret_key", &secret_key_json))
        secret_key_json.AsString(&secret_key_);
    if (cred_json.Find("key_id", &key_id_json))
        key_id_json.AsString(&key_id_);
    if (cred_json.Find("region", &region_json))
        region_json.AsString(&region_);
    if (cred_json.Find("session_token", &session_token_json))
        session_token_json.AsString(&session_token_);
    if (cred_json.Find("profile", &profile_json))
        profile_json.AsString(&profile_name_);
}

class FileSystem {
public:
    FileSystem(const std::string& s3_path, const S3Credential& s3_cred);
    TRITONSERVER_Error* FileExists(const std::string& path, bool* exists);
    TRITONSERVER_Error* IsDirectory(const std::string& path, bool* is_dir);
    TRITONSERVER_Error* GetDirectoryContents(const std::string& path, std::set<std::string>* contents) ;
    TRITONSERVER_Error* ParsePath(const std::string& path, std::string* bucket, std::string* object);
    TRITONSERVER_Error* CleanPath(const std::string& s3_path, std::string* clean_path);

    TRITONSERVER_Error* LocalizePath(
            const std::string& path,
            const std::string& l);

private:
    std::unique_ptr<s3::S3Client> client_;  // init after Aws::InitAPI is called
};

  FileSystem::FileSystem(const std::string& s3_path, const S3Credential& s3_cred) {
      // init aws api if not already
      Aws::SDKOptions options;
      Aws::InitAPI(options);

      // check vars for S3 credentials -> aws profile -> default
      if (!s3_cred.secret_key_.empty() && !s3_cred.key_id_.empty()) {
          credentials.SetAWSAccessKeyId(s3_cred.key_id_.c_str());
          credentials.SetAWSSecretKey(s3_cred.secret_key_.c_str());
          if (!s3_cred.session_token_.empty()) {
              credentials.SetSessionToken(s3_cred.session_token_.c_str());
          }
          config = Aws::Client::ClientConfiguration();
          if (!s3_cred.region_.empty()) {
              config.region = s3_cred.region_.c_str();
          }
      } else if (!s3_cred.profile_name_.empty()) {
          config = Aws::Client::ClientConfiguration(s3_cred.profile_name_.c_str());
      } else {
          config = Aws::Client::ClientConfiguration("default");
      }

      if (!s3_cred.secret_key_.empty() && !s3_cred.key_id_.empty()) {
          client_ = std::make_unique<s3::S3Client>(
                  credentials, config,
                  Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,false);
      } else {
          client_ = std::make_unique<s3::S3Client>(
                  config, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,false);
      }
  }

  TRITONSERVER_Error*
  FileSystem::CleanPath(const std::string &s3_path, std::string *clean_path) {
      // Must handle paths with s3 prefix
      size_t start = s3_path.find("s3://");
      std::string path = "";
      if (start != std::string::npos) {
          path = s3_path.substr(start + strlen("s3://"));
          *clean_path = "s3://";
      } else {
          path = s3_path;
          *clean_path = "";
      }

      // Must handle paths with https:// or http:// prefix
      size_t https_start = path.find("https://");
      if (https_start != std::string::npos) {
          path = path.substr(https_start + strlen("https://"));
          *clean_path += "https://";
      } else {
          size_t http_start = path.find("http://");
          if (http_start != std::string::npos) {
              path = path.substr(http_start + strlen("http://"));
              *clean_path += "http://";
          }
      }

      // Remove trailing slashes
      size_t rtrim_length = path.find_last_not_of('/');
      if (rtrim_length == std::string::npos) {
          return TRITONSERVER_ErrorNew((TRITONSERVER_ERROR_INVALID_ARG, "Invalid bucket name: '" + path + "'");
      }

      // Remove leading slashes
      size_t ltrim_length = path.find_first_not_of('/');
      if (ltrim_length == std::string::npos) {
          return TRITONSERVER_ErrorNew((TRITONSERVER_ERROR_INVALID_ARG,  "Invalid bucket name: '" + path + "'");
      }

      // Remove extra internal slashes
      std::string true_path = path.substr(ltrim_length, rtrim_length + 1);
      bool previous_slash = false;
      for (size_t i = 0; i < true_path.size(); i++) {
          if (true_path[i] == '/') {
              if (!previous_slash) {
                  *clean_path += true_path[i];
              }
              previous_slash = true;
          } else {
              *clean_path += true_path[i];
              previous_slash = false;
          }
      }

      return nullptr;
  }

  TRITONSERVER_Error*
  FileSystem::ParsePath(const std::string &path, std::string *bucket, std::string *object) {
      // Cleanup extra slashes
      std::string clean_path;
      RETURN_IF_ERROR(CleanPath(path, &clean_path));

      // Get the bucket name and the object path. Return error if path is malformed
      std::string protocol, host_name, host_port;
      if (!RE2::FullMatch(clean_path, s3_regex_, &protocol, &host_name, &host_port, bucket, object)) {
          int bucket_start = clean_path.find("s3://") + strlen("s3://");
          int bucket_end = clean_path.find("/", bucket_start);

          // If there isn't a slash, the address has only the bucket
          if (bucket_end > bucket_start) {
              *bucket = clean_path.substr(bucket_start, bucket_end - bucket_start);
              *object = clean_path.substr(bucket_end + 1);
          } else {
              *bucket = clean_path.substr(bucket_start);
              *object = "";
          }
      } else {
          // Erase leading '/' that is left behind in object name
          if ((*object)[0] == '/') {
              object->erase(0, 1);
          }
      }

      if (bucket->empty()) {
          TRITONSERVER_ErrorNew((TRITONSERVER_ERROR_INTERNAL, "No bucket name found in path: " + path);
      }

      return nullptr;
  }
//S3FileSystem::
TRITONSERVER_Error*
  FileSystem::IsDirectory(const std::string& path, bool* is_dir)
  {
      *is_dir = false;
      std::string bucket, object_path;
      RETURN_IF_ERROR(ParsePath(path, &bucket, &object_path));

      // Check if the bucket exists
      s3::Model::HeadBucketRequest head_request;
      head_request.WithBucket(bucket.c_str());

      auto head_bucket_outcome = client_->HeadBucket(head_request);
      if (!head_bucket_outcome.IsSuccess()) {
          return TRITONSERVER_ErrorNew(
                  TRITONSERVER_ERROR_INTERNAL,
                  "Could not get MetaData for bucket with name " + bucket +
                  " due to exception: " +
                  head_bucket_outcome.GetError().GetExceptionName() +
                  ", error message: " + head_bucket_outcome.GetError().GetMessage());
      }

      // Root case - bucket exists and object path is empty
      if (object_path.empty()) {
          *is_dir = true;
          return nullptr;
      }

      // List the objects in the bucket
      s3::Model::ListObjectsV2Request list_objects_request;
      list_objects_request.SetBucket(bucket.c_str());
      list_objects_request.SetPrefix(AppendSlash(object_path).c_str());
      auto list_objects_outcome = client_->ListObjectsV2(list_objects_request);

      if (list_objects_outcome.IsSuccess()) {
          *is_dir = !list_objects_outcome.GetResult().GetContents().empty();
      } else {
          return TRITONSERVER_ErrorNew(TRITON_ERROR_INTERNAL,"Failed to list objects with prefix " + path + " due to exception: " +
                    list_objects_outcome.GetError().GetExceptionName() +
                    ", error message: " + list_objects_outcome.GetError().GetMessage());
        }
        return nullptr;
    }

    TRITONSERVER_Error*
    FileSystem::FileExists(const std::string &path, bool *exists) {
        *exists = false;

        // S3 doesn't make objects for directories, so it could still be a directory
        bool is_dir;
        RETURN_IF_ERROR(IsDirectory(path, &is_dir));
        if (is_dir) {
            *exists = is_dir;
            return nullptr;
        }

        std::string bucket, object;
        RETURN_IF_ERROR(ParsePath(path, &bucket, &object));

        // Construct request for object metadata
        s3::Model::HeadObjectRequest head_request;
        head_request.SetBucket(bucket.c_str());
        head_request.SetKey(object.c_str());

        auto head_object_outcome = client_->HeadObject(head_request);
        if (!head_object_outcome.IsSuccess()) {
            if (head_object_outcome.GetError().GetErrorType() !=
            s3::S3Errors::RESOURCE_NOT_FOUND) {
                return TRITONSERVER_ErrorNew(
                        TRITONSERVER_ERROR_INTERNAL,
                                    "Could not get MetaData for object at " + path +
                                    " due to exception: " +
                                    head_object_outcome.GetError().GetExceptionName() +
                                    ", error message: " +
                                    head_object_outcome.GetError().GetMessage());
            }
        } else {
            *exists = true;
        }

        return nullptr;
  }

  std::string
  AppendSlash(const std::string& name)
  {
      if (name.empty() || (name.back() == '/')) {
          return name;
      }

      return (name + "/");
  }

  TRITONSERVER_Error*
  FileSystem::GetDirectoryContents(const std::string& path, std::set<std::string>* contents)
  {
      // Parse bucket and dir_path
      std::string bucket, dir_path, full_dir;
      RETURN_IF_ERROR(ParsePath(path, &bucket, &dir_path));
      std::string true_path = "s3://" + bucket + '/' + dir_path;

      // Capture the full path to facilitate content listing
      full_dir = AppendSlash(dir_path);

      // Issue request for objects with prefix
      s3::Model::ListObjectsV2Request objects_request;
      objects_request.SetBucket(bucket.c_str());
      objects_request.SetPrefix(full_dir.c_str());

      bool done_listing = false;
      while (!done_listing) {
          auto list_objects_outcome = client_->ListObjectsV2(objects_request);

          if (!list_objects_outcome.IsSuccess()) {
              return TRITONSERVER_ErrorNew(
                      TRITONSERVER_ERROR_INTERNAL,
                      "Could not list contents of directory at " + true_path +
                      " due to exception: " +
                      list_objects_outcome.GetError().GetExceptionName() +
                      ", error message: " +
                      list_objects_outcome.GetError().GetMessage());
          }
          const auto& list_objects_result = list_objects_outcome.GetResult();
          for (const auto& s3_object : list_objects_result.GetContents()) {
              // In the case of empty directories, the directory itself will appear here
              if (s3_object.GetKey().c_str() == full_dir) {
                  continue;
              }

              // We have to make sure that subdirectory contents do not appear here
              std::string name(s3_object.GetKey().c_str());
              int item_start = name.find(full_dir) + full_dir.size();
              // S3 response prepends parent directory name
              int item_end = name.find("/", item_start);

              // Let set take care of subdirectory contents
              std::string item = name.substr(item_start, item_end - item_start);
              contents->insert(item);

              // Fail-safe check to ensure the item name is not empty
              if (item.empty()) {
                  return TRITONSERVER_ErrorNew(
                          TRITONSERVER_ERROR_INTERNAL,
                          "Cannot handle item with empty name at " + true_path);
              }
          }
          // If there are more pages to retrieve, set the marker to the next page.
          if (list_objects_result.GetIsTruncated()) {
              objects_request.SetContinuationToken(
                      list_objects_result.GetNextContinuationToken());
          } else {
              done_listing = true;
          }
      }
      return nullptr;
  }

  class LocalizedPath {
  public:
      LocalizedPath(const std::string& original_path): original_path_(original_path) { }
      LocalizedPath(const std::string& original_path, const std::string& local_path)
      : original_path_(original_path), local_path_(local_path) { }
      ~LocalizedPath();

      const std::string& Path() const {
          return (local_path_.empty()) ? original_path_ : local_path_;
      }
  private:
      std::string original_path_;
      std::string local_path_;
  };

  TRITONSERVER_Error*
  FileSystem::LocalizePath(const std::string& path, const std::string& tmp_folder) {
  // Check if the directory or file exists
  bool exists;
  RETURN_IF_ERROR(FileExists(path, &exists));
  if (!exists) {
          return TRITONSERVER_ErrorNew(
                  TRITONSERVER_ERROR_INTERNAL, "directory or file does not exist at " + path);
  }

  // Cleanup extra slashes
  std::string clean_path;
  RETURN_IF_ERROR(CleanPath(path, &clean_path));
  // Remove protocol and host name and port
  std::string effective_path, protocol, host_name, host_port, bucket, object;
  re2::RE2  s3_regex_("s3://(http://|https://|)([0-9a-zA-Z\\-.]+):([0-9]+)/"
          "([0-9a-z.\\-]+)(((/[0-9a-zA-Z.\\-_]+)*)?)")
  if (RE2::FullMatch(clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,&object)) {
      effective_path = "s3://" + bucket + object;
  } else {
      effective_path = path;
  }

  // Specify contents to be downloaded
  std::set<std::string> contents;
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(path, &is_dir));
  if (is_dir) {
      // Set localized path
      localized->reset(new LocalizedPath(effective_path, tmp_folder));
      // Specify the entire directory to be downloaded
      std::set<std::string> filenames;
      RETURN_IF_ERROR(GetDirectoryContents(effective_path, &filenames));
      for (auto itr = filenames.begin(); itr != filenames.end(); ++itr) {
          contents.insert(JoinPath({effective_path, *itr}));
      }
  } else {
      std::string filename =
                  effective_path.substr(effective_path.find_last_of('/') + 1);
      localized->reset(
                  new LocalizedPath(effective_path, JoinPath({tmp_folder, filename})));
      contents.insert(effective_path);
  }

  // Download all specified contents and nested contents
  while (contents.size() != 0) {
      std::set<std::string> tmp_contents = contents;
      contents.clear();
      for (auto iter = tmp_contents.begin(); iter != tmp_contents.end(); ++iter) {
          std::string s3_fpath = *iter;
          std::string s3_removed_path = s3_fpath.substr(effective_path.size());
          std::string local_fpath = s3_removed_path.empty() ? (*localized)->Path()
                  : JoinPath({(*localized)->Path(), s3_removed_path});
          bool is_subdir;
          RETURN_IF_ERROR(IsDirectory(s3_fpath, &is_subdir));
          if (is_subdir) {
              // Create local mirror of sub-directories
#ifdef _WIN32
            int status = mkdir(const_cast<char*>(local_fpath.c_str()));
#else
            int status = mkdir(const_cast<char*>(local_fpath.c_str()), S_IRUSR | S_IWUSR | S_IXUSR);
#endif
            if (status == -1) {
                return TRITONSERVER_ErrorNew(TRITONSERVER_ERROR_INTERNAL,"Failed to create local folder: " + local_fpath +
                    ", errno:" + strerror(errno));
            }

            // Add sub-directories and deeper files to contents
            std::set<std::string> subdir_contents;
            RETURN_IF_ERROR(GetDirectoryContents(s3_fpath, &subdir_contents));
            for (auto itr = subdir_contents.begin(); itr != subdir_contents.end();++itr) {
                                    contents.insert(JoinPath({s3_fpath, *itr}));
            }
          } else {
              // Create local copy of file
              std::string file_bucket, file_object;
              RETURN_IF_ERROR(ParsePath(s3_fpath, &file_bucket, &file_object));

              aws::string URL_aws;
              URL_aws = client_->GeneratePresignedUrl(
                      aws_s(file_bucket.c_str(), file_bucket.size()), aws_s(file_object.c_str(), file_object.size()), HTTP_GET, expiresInSec);
              std::string URL(URL_aws.c_str(), URL_aws.size());

              httplib::Client cli("localhost", 65001);
              auto res = cli.Get(URL);
              if(res->status == 200) {
                  auto& retrieved_file = res->body;
                  std::ofstream output_file(local_fpath.c_str(), std::ios::binary);
                  output_file << retrieved_file.rdbuf();
                  output_file.close;
              } else {
                  TRITONSERVER_ErrorNew(
                          TRITONSERVER_ERROR_INTERNAL, "Failed to get object at " + s3_fpath + " due to exception: " +
                          res.error());
              }
          }
      }
  }
    return nullptr;
  }
}  // namespace

extern "C" {
    TRITONSERVER_Error*
    TRITONREPOAGENT_ModelAction(
            TRITONREPOAGENT_Agent* agent, TRITONREPOAGENT_AgentModel* model,
            const TRITONREPOAGENT_ActionType action_type)
    {
        // Return success (nullptr) if the agent does not handle the action
        if (action_type != TRITONREPOAGENT_ACTION_LOAD) {
            return nullptr;
        }

        const char* location_cstr;
        TRITONREPOAGENT_ArtifactType artifact_type;
        RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocation(
                        agent, model, &artifact_type, &location_cstr));
        const std::string location(location_cstr);

        const char* tmp_cstr;
        RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocationAcquire(
                agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, &tmp_cstr));
        const std::string tmp_folder(tmp_cstr);

        FileSystem fs(location, S3Credential());
        RETURN_IF_ERROR(fs.LocalizePath(location, tmp_folder))

        RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryUpdate(
                agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, location));

        return nullptr;
    }

}  // extern "C"

}}}  // namespace triton::repoagent::dragonfly
