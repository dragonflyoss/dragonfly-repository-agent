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
#include "api.h"
#include "implementations/common.h"
#include "triton/core/tritonserver.h"
#include "common_utils.h"
#include "config.h"

#define TRITON_ENABLE_GCS
#define TRITON_ENABLE_S3
#define TRITON_ENABLE_AZURE_STORAGE

#ifdef TRITON_ENABLE_GCS
#include "implementations/gcs.h"
#endif  // TRITON_ENABLE_GCS

#ifdef TRITON_ENABLE_S3
#include "implementations/s3.h"
#endif  // TRITON_ENABLE_S3

#ifdef TRITON_ENABLE_AZURE_STORAGE
#include "implementations/as.h"
#endif  // TRITON_ENABLE_AZURE_STORAGE



#include <mutex>

namespace triton::repoagent::dragonfly {

namespace {

class FileSystemManager {
 public:
  TRITONSERVER_Error* GetFileSystem(
      const std::string& path,
      std::shared_ptr<FileSystem>& file_system,
      const std::string& cred_path);

  // 创建file_system
 private:
  template<class CacheType, class CredentialType, class FileSystemType>
  TRITONSERVER_Error* GetFileSystem(
      const std::string& path,
      CacheType& cache,
      std::shared_ptr<FileSystem>& file_system,
      const std::string& cred_path);

  TRITONSERVER_Error* LoadCredentials(const std::string& cred_path);

  template<class CacheType, class CredentialType, class FileSystemType>
  static void LoadCredential(
      triton::common::TritonJson::Value &creds_json,
      const char *fs_type,
      CacheType &cache);

  template <class CredentialType, class FileSystemType>
  static void SortCache(std::vector<std::tuple<
                            std::string, CredentialType,
                            std::shared_ptr<FileSystemType>>>& cache);

  template <class CredentialType, class FileSystemType>
  static TRITONSERVER_Error* GetLongestMatchingNameIndex(
      const std::vector<std::tuple<
          std::string, CredentialType, std::shared_ptr<FileSystemType>>>& cache,
      const std::string& path,
      size_t& idx);

#ifdef TRITON_ENABLE_GCS
  std::vector<
      std::tuple<std::string, GCSCredential, std::shared_ptr<GCSFileSystem>>>
      gs_cache_;
#endif  // TRITON_ENABLE_GCS
#ifdef TRITON_ENABLE_S3
  std::vector<
      std::tuple<std::string, S3Credential, std::shared_ptr<S3FileSystem>>>
      s3_cache_;
#endif  // TRITON_ENABLE_S3
#ifdef TRITON_ENABLE_AZURE_STORAGE
  std::vector<
      std::tuple<std::string, ASCredential, std::shared_ptr<ASFileSystem>>>
      as_cache_;
#endif  // TRITON_ENABLE_AZURE_STORAGE
};

TRITONSERVER_Error*
FileSystemManager::GetFileSystem(
    const std::string& path, std::shared_ptr<FileSystem>& file_system, const std::string& cred_path) {
  // Check if this is a GCS path (gs://$BUCKET_NAME)
  if (!path.empty() && !path.rfind("gs://", 0)) {
#ifndef TRITON_ENABLE_GCS
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "gs:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_GCS=ON.");
#else
    return GetFileSystem<
        std::vector<std::tuple<std::string, GCSCredential, std::shared_ptr<GCSFileSystem>>>,
        GCSCredential, GCSFileSystem>(path, gs_cache_, file_system, cred_path);
#endif  // TRITON_ENABLE_GCS
  }

  // Check if this is an S3 path (s3://$BUCKET_NAME)
  if (!path.empty() && !path.rfind("s3://", 0)) {
#ifndef TRITON_ENABLE_S3
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "s3:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_S3=ON.");
#else
    return GetFileSystem<
        std::vector<std::tuple<
            std::string, S3Credential, std::shared_ptr<S3FileSystem>>>,
        S3Credential, S3FileSystem>(path, s3_cache_, file_system, cred_path);
#endif  // TRITON_ENABLE_S3
  }

  // Check if this is an Azure Storage path
  if (!path.empty() && !path.rfind("as://", 0)) {
#ifndef TRITON_ENABLE_AZURE_STORAGE
    return TRITONSERVER_ErrorNew(
        TRITONSERVER_ERROR_UNSUPPORTED,
        "as:// file-system not supported. To enable, build with "
        "-DTRITON_ENABLE_AZURE_STORAGE=ON.");
#else
    return GetFileSystem<
        std::vector<std::tuple<
            std::string, ASCredential, std::shared_ptr<ASFileSystem>>>,
        ASCredential, ASFileSystem>(path, as_cache_, file_system, cred_path);
#endif  // TRITON_ENABLE_AZURE_STORAGE
  }

  return TRITONSERVER_ErrorNew(
      TRITONSERVER_ERROR_UNSUPPORTED,
      "filesystem type error");
}

TRITONSERVER_Error*
FileSystemManager::LoadCredentials(const std::string& cred_path)
{
  // 从 cred_path 获取配置文件
  triton::common::TritonJson::Value creds_json;
  std::string cred_file_content;
  RETURN_IF_ERROR(ReadLocalFile(cred_path, &cred_file_content));
  RETURN_IF_ERROR(creds_json.Parse(cred_file_content));

  // 根据文件系统，解析
#ifdef TRITON_ENABLE_GCS
  // load GCS credentials
  LoadCredential<
      std::vector<std::tuple<
          std::string, GCSCredential, std::shared_ptr<GCSFileSystem>>>,
      GCSCredential, GCSFileSystem>(creds_json, "gs", gs_cache_);
#endif  // TRITON_ENABLE_GCS
#ifdef TRITON_ENABLE_S3
  // load S3 credentials
  LoadCredential<
      std::vector<std::tuple<
          std::string, S3Credential, std::shared_ptr<S3FileSystem>>>,
      S3Credential, S3FileSystem>(creds_json, "s3", s3_cache_);
#endif  // TRITON_ENABLE_S3
#ifdef TRITON_ENABLE_AZURE_STORAGE
  // load AS credentials
  LoadCredential<
      std::vector<std::tuple<
          std::string, ASCredential, std::shared_ptr<ASFileSystem>>>,
      ASCredential, ASFileSystem>(creds_json, "as", as_cache_);
#endif  // TRITON_ENABLE_AZURE_STORAGE
  return nullptr;
}


template <class CacheType, class CredentialType, class FileSystemType>
void
FileSystemManager::LoadCredential(
    triton::common::TritonJson::Value& creds_json, const char* fs_type,
    CacheType& cache)
{
  cache.clear();
  triton::common::TritonJson::Value creds_fs_json;
  if (creds_json.Find(fs_type, &creds_fs_json)) {
    std::vector<std::string> cred_names;
    creds_fs_json.Members(&cred_names);
    for (size_t i = 0; i < cred_names.size(); i++) {
      std::string cred_name = cred_names[i];
      triton::common::TritonJson::Value cred_json;
      creds_fs_json.Find(cred_name.c_str(), &cred_json);
      ASCredential as(cred_json);
      cache.push_back(std::make_tuple(
          cred_name, CredentialType(cred_json),
          std::shared_ptr<FileSystemType>()));
    }
    SortCache(cache);
  }
}

// 根据filesystem的类型进行处理
template <class CacheType, class CredentialType, class FileSystemType>
TRITONSERVER_Error*
FileSystemManager::GetFileSystem(
    const std::string& path, CacheType& cache,
    std::shared_ptr<FileSystem>& file_system,
    const std::string& cred_path)
{
  RETURN_IF_ERROR(LoadCredentials(cred_path));

  size_t idx;
  RETURN_IF_ERROR(GetLongestMatchingNameIndex(cache, path, idx));
  CredentialType cred = std::get<1>(cache[idx]);

  std::shared_ptr<FileSystemType> fs = std::make_shared<FileSystemType>(path, cred);
  RETURN_IF_ERROR(fs->CheckClient(path));
  file_system = fs;
  return nullptr;
}

template <class CredentialType, class FileSystemType>
void
FileSystemManager::SortCache(std::vector<std::tuple<
                                 std::string, CredentialType, std::shared_ptr<FileSystemType>>>& cache)
{
  std::sort(
      cache.begin(), cache.end(),
      [](std::tuple<
             std::string, CredentialType, std::shared_ptr<FileSystemType>>
             a,
         std::tuple<
             std::string, CredentialType, std::shared_ptr<FileSystemType>>
             b) { return std::get<0>(a).size() >= std::get<0>(b).size(); });
}

template <class CredentialType, class FileSystemType>
TRITONSERVER_Error*
FileSystemManager::GetLongestMatchingNameIndex(
    const std::vector<std::tuple<
        std::string, CredentialType, std::shared_ptr<FileSystemType>>>& cache,
    const std::string& path, size_t& idx)
{
  for (size_t i = 0; i < cache.size(); i++) {
    if (!path.rfind(std::get<0>(cache[i]), 0)) {
      idx = i;
      return nullptr;
    }
  }
  return TRITONSERVER_ErrorNew(
      TRITONSERVER_ERROR_NOT_FOUND,
      ("Cannot match credential for path  " + path).c_str());
}

FileSystemManager fsm_;
}

TRITONSERVER_Error*
LocalizePath(const std::string& config_path, const std::string& cred_path,
             const std::string& location, const std::string& temp_dir)
{
  std::shared_ptr<FileSystem> fs;
  RETURN_IF_ERROR(fsm_.GetFileSystem(location, fs, cred_path));

  std::string config_file_content;
  RETURN_IF_ERROR(ReadLocalFile(config_path, &config_file_content));
  triton::common::TritonJson::Value config_json;
  RETURN_IF_ERROR(config_json.Parse(config_file_content));
  DragonflyConfig config(config_json);

  return fs->LocalizePath(location, temp_dir, config);
}

}
