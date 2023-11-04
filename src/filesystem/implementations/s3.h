/*
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

#include "aws/core/Aws.h"
#include "aws/core/auth/AWSCredentialsProvider.h"
#include "aws/core/client/ClientConfiguration.h"
#include "aws/core/http/curl/CurlHttpClient.h"
#include "aws/core/http/standard/StandardHttpRequest.h"
#include "aws/s3/S3Client.h"
#include "aws/s3/model/HeadBucketRequest.h"
#include "aws/s3/model/HeadObjectRequest.h"
#include "aws/s3/model/ListObjectsV2Request.h"
#include "aws/s3/model/ListObjectsV2Result.h"
#include "common.h"
#include "common_utils.h"
#include "fstream"
#include "iostream"
#include "re2/re2.h"
#include "sys/stat.h"
#include "triton/core/tritonserver.h"

namespace triton::repoagent::dragonfly {

namespace s3 = Aws::S3;

// Override the default S3 Curl initialization for disabling HTTP/2 on s3.
// Remove once s3 fully supports HTTP/2 [FIXME: DLIS-4973].
// Reference:
// https://github.com/awsdocs/aws-doc-sdk-examples/blob/main/cpp/example_code/s3/list_buckets_disabling_dns_cache.cpp
static const char S3_ALLOCATION_TAG[] = "OverrideDefaultHttpClient";
class S3CurlHttpClient : public Aws::Http::CurlHttpClient {
 public:
  explicit S3CurlHttpClient(
      const Aws::Client::ClientConfiguration& client_config)
      : Aws::Http::CurlHttpClient(client_config)
  {
  }

 protected:
  void OverrideOptionsOnConnectionHandle(CURL* connectionHandle) const override
  {
    curl_easy_setopt(
        connectionHandle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  }
};
class S3HttpClientFactory : public Aws::Http::HttpClientFactory {
  [[nodiscard]] std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(
      const Aws::Client::ClientConfiguration& client_config) const override
  {
    return Aws::MakeShared<S3CurlHttpClient>(S3_ALLOCATION_TAG, client_config);
  }
  [[nodiscard]] std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
      const Aws::String& uri, Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& stream_factory) const override
  {
    return CreateHttpRequest(Aws::Http::URI(uri), method, stream_factory);
  }
  [[nodiscard]] std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
      const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& stream_factory) const override
  {
    auto req = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
        S3_ALLOCATION_TAG, uri, method);
    req->SetResponseStreamFactory(stream_factory);
    return req;
  }
  void InitStaticState() override { S3CurlHttpClient::InitGlobalState(); }
  void CleanupStaticState() override { S3CurlHttpClient::CleanupGlobalState(); }
};

struct S3Credential {
  std::string secret_key_;
  std::string key_id_;
  std::string region_;
  std::string session_token_;
  std::string profile_name_;

  explicit S3Credential(triton::common::TritonJson::Value& cred_json);
};

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

class S3FileSystem : public FileSystem {
 public:
  S3FileSystem(const std::string& s3_path, const S3Credential& s3_cred);

  TRITONSERVER_Error* LocalizePath(
      const std::string& location, const std::string& temp_dir,
      DragonflyConfig& config) override;

  TRITONSERVER_Error* CheckClient(const std::string& s3_path);

 private:
  TRITONSERVER_Error* FileExists(const std::string& path, bool* exists);
  TRITONSERVER_Error* IsDirectory(const std::string& path, bool* is_dir);
  TRITONSERVER_Error* GetDirectoryContents(
      const std::string& path, std::set<std::string>* contents);
  TRITONSERVER_Error* ParsePath(
      const std::string& path, std::string* bucket, std::string* object);
  static TRITONSERVER_Error* CleanPath(
      const std::string& s3_path, std::string* clean_path);
  std::unique_ptr<s3::S3Client> client_;  // init after Aws::InitAPI is called
  re2::RE2 s3_regex_;
};

TRITONSERVER_Error*
S3FileSystem::ParsePath(
    const std::string& path, std::string* bucket, std::string* object)
{
  // Cleanup extra slashes
  std::string clean_path;
  RETURN_IF_ERROR(CleanPath(path, &clean_path));

  // Get the bucket name and the object path. Return error if path is malformed
  std::string protocol, host_name, host_port;
  if (!RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, bucket,
          object)) {
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
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("No bucket name found in path: " + path).c_str());
  }

  return nullptr;
}

// Clean and normalize the S3 path
TRITONSERVER_Error*
S3FileSystem::CleanPath(const std::string& s3_path, std::string* clean_path)
{
  // Must handle paths with s3 prefix
  size_t start = s3_path.find("s3://");
  std::string path;
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
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        ("Invalid bucket name: '" + path + "'").c_str());
  }

  // Remove leading slashes
  size_t ltrim_length = path.find_first_not_of('/');
  if (ltrim_length == std::string::npos) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INVALID_ARG,
        ("Invalid bucket name: '" + path + "'").c_str());
  }

  // Remove extra internal slashes
  std::string true_path = path.substr(ltrim_length, rtrim_length + 1);
  std::vector<int> slash_locations;
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

S3FileSystem::S3FileSystem(
    const std::string& s3_path, const S3Credential& s3_cred)
    : s3_regex_(
          "s3://(http://|https://|)([0-9a-zA-Z\\-.]+):([0-9]+)/"
          "([0-9a-z.\\-]+)(((/[0-9a-zA-Z.\\-_]+)*)?)")
{
  // init aws api if not already
  Aws::SDKOptions options;
  static std::once_flag onceFlag;
  std::call_once(onceFlag, [&options] { Aws::InitAPI(options); });

  // [FIXME: DLIS-4973]
  Aws::Http::SetHttpClientFactory(
      Aws::MakeShared<S3HttpClientFactory>(S3_ALLOCATION_TAG));

  Aws::Client::ClientConfiguration config;
  Aws::Auth::AWSCredentials credentials;

  // check vars for S3 credentials -> aws profile -> default
  if (!s3_cred.secret_key_.empty() && !s3_cred.key_id_.empty()) {
    credentials.SetAWSAccessKeyId(s3_cred.key_id_.c_str());
    credentials.SetAWSSecretKey(s3_cred.secret_key_.c_str());
    if (!s3_cred.session_token_.empty()) {
      credentials.SetSessionToken(s3_cred.session_token_.c_str());
    }
    config = Aws::Client::ClientConfiguration();
    if (!s3_cred.region_.empty()) {
      config.region = s3_cred.region_;
    }
  } else if (!s3_cred.profile_name_.empty()) {
    config = Aws::Client::ClientConfiguration(s3_cred.profile_name_.c_str());
  } else {
    config = Aws::Client::ClientConfiguration("default");
  }

  // Cleanup extra slashes
  std::string clean_path;
  TRITONSERVER_Error* err = CleanPath(s3_path, &clean_path);
  if (err != nullptr) {
    TRITONSERVER_ErrorDelete(err);
    throw std::runtime_error("failed to parse S3 path");
  }

  std::string protocol, host_name, host_port, bucket, object;
  if (RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,
          &object)) {
    config.endpointOverride = Aws::String(host_name + ":" + host_port);
    if (protocol == "https://") {
      config.scheme = Aws::Http::Scheme::HTTPS;
    } else {
      config.scheme = Aws::Http::Scheme::HTTP;
    }
  }

  if (!s3_cred.secret_key_.empty() && !s3_cred.key_id_.empty()) {
    client_ = std::make_unique<s3::S3Client>(
        credentials, config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAdressing*/ false);
  } else {
    client_ = std::make_unique<s3::S3Client>(
        config, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAdressing*/ false);
  }
}

TRITONSERVER_Error*
S3FileSystem::CheckClient(const std::string& s3_path)
{
  std::string bucket, object_path;
  RETURN_IF_ERROR(ParsePath(s3_path, &bucket, &object_path));
  // check if can connect to the bucket
  s3::Model::HeadBucketRequest head_request;
  head_request.WithBucket(bucket.c_str());
  auto head_object_outcome = client_->HeadBucket(head_request);
  if (!head_object_outcome.IsSuccess()) {
    auto err = head_object_outcome.GetError();
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Unable to create S3 filesystem client. Check account credentials. "
         "Exception: '" +
         err.GetExceptionName() + "' Message: '" + err.GetMessage() + "'")
            .c_str());
  }
  return nullptr;
}

TRITONSERVER_Error*
S3FileSystem::FileExists(const std::string& path, bool* exists)
{
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
          ("Could not get MetaData for object at " + path +
           " due to exception: " +
           head_object_outcome.GetError().GetExceptionName() +
           ", error message: " + head_object_outcome.GetError().GetMessage())
              .c_str());
    }
  } else {
    *exists = true;
  }

  return nullptr;
}

TRITONSERVER_Error*
S3FileSystem::IsDirectory(const std::string& path, bool* is_dir)
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
        ("Could not get MetaData for bucket with name " + bucket +
         " due to exception: " +
         head_bucket_outcome.GetError().GetExceptionName() +
         ", error message: " + head_bucket_outcome.GetError().GetMessage())
            .c_str());
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
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("Failed to list objects with prefix " + path + " due to exception: " +
         list_objects_outcome.GetError().GetExceptionName() +
         ", error message: " + list_objects_outcome.GetError().GetMessage())
            .c_str());
  }
  return nullptr;
}

TRITONSERVER_Error*
S3FileSystem::GetDirectoryContents(
    const std::string& path, std::set<std::string>* contents)
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
          ("Could not list contents of directory at " + true_path +
           " due to exception: " +
           list_objects_outcome.GetError().GetExceptionName() +
           ", error message: " + list_objects_outcome.GetError().GetMessage())
              .c_str());
    }
    const auto& list_objects_result = list_objects_outcome.GetResult();
    for (const auto& s3_object : list_objects_result.GetContents()) {
      // In the case of empty directories, the directory itself will appear
      // here
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
            ("Cannot handle item with empty name at " + true_path).c_str());
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

TRITONSERVER_Error*
S3FileSystem::LocalizePath(
    const std::string& location, const std::string& temp_dir,
    DragonflyConfig& config)
{
  bool exists;
  RETURN_IF_ERROR(FileExists(location, &exists));
  if (!exists) {
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_INTERNAL,
        ("directory or file does not exist at " + location).c_str());
  }

  std::string clean_path;
  RETURN_IF_ERROR(CleanPath(location, &clean_path));

  std::string effective_path, protocol, host_name, host_port, bucket, object;
  if (RE2::FullMatch(
          clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,
          &object)) {
    effective_path = "s3://" + bucket + object;
  } else {
    effective_path = location;
  }

  std::set<std::string> contents;
  bool is_dir;
  RETURN_IF_ERROR(IsDirectory(location, &is_dir));
  if (is_dir) {
    std::set<std::string> filenames;
    RETURN_IF_ERROR(GetDirectoryContents(effective_path, &filenames));
    for (auto itr = filenames.begin(); itr != filenames.end(); ++itr) {
      contents.insert(JoinPath({effective_path, *itr}));
    }
  } else {
    std::string filename =
        effective_path.substr(effective_path.find_last_of('/') + 1);
    std::string local_file_path = JoinPath({temp_dir, filename});
    contents.insert(local_file_path);
  }

  while (!contents.empty()) {
    std::set<std::string> tmp_contents = contents;
    contents.clear();
    for (const auto& s3_fpath : tmp_contents) {
      std::string s3_removed_path = s3_fpath.substr(effective_path.size());
      std::string local_fpath = s3_removed_path.empty()
                                    ? temp_dir
                                    : JoinPath({temp_dir, s3_removed_path});
      bool is_subdir;
      RETURN_IF_ERROR(IsDirectory(s3_fpath, &is_subdir));
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
        RETURN_IF_ERROR(GetDirectoryContents(s3_fpath, &subdir_contents));
        for (const auto& subdir_content : subdir_contents) {
          contents.insert(JoinPath({s3_fpath, subdir_content}));
        }
      } else {
        std::string file_bucket, file_object;
        RETURN_IF_ERROR(ParsePath(s3_fpath, &file_bucket, &file_object));

        std::string url = client_->GeneratePresignedUrl(
            file_bucket, file_object, Aws::Http::HttpMethod::HTTP_GET);
        RETURN_IF_ERROR(DownloadFile(url, local_fpath, config));
      }
    }
  }

  return nullptr;
}

}  // namespace triton::repoagent::dragonfly
