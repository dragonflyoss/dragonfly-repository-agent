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

#include "triton/core/tritonrepoagent.h"
#include "triton/core/tritonserver.h"
#include "aws/core/Aws.h"
#include "aws/s3/S3Client.h"
#include "httplib.h"z

const uint64_t expiresInSec = 3600;

namespace triton { namespace repoagent { namespace dragonfly {

namespace {
//
// ErrorException
//
// Exception thrown if error occurs while running DragonflyRepoAgent
//
struct ErrorException {
  ErrorException(TRITONSERVER_Error* err) : err_(err) {}
  TRITONSERVER_Error* err_;
};

#define THROW_IF_TRITON_ERROR(X)                                    
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
  } while (false)                        \


class LocalizedPath {
public:
    // Create an object for a path that is already local.
    LocalizedPath(const std::string& original_path)
    : original_path_(original_path) {}

    // Create an object for a remote path. Store both the original path and the
    // temporary local path.
    LocalizedPath(const std::string& original_path, const std::string& local_path)
    : original_path_(original_path), local_path_(local_path) { }

    // Destructor. Remove temporary local storage associated with the object.
    // If the local path is a directory, delete the directory.
    // If the local path is a file, delete the directory containing the file.
    ~LocalizedPath() {
        if (!local_path_.empty()) {
            bool is_dir = true;
            IsDirectory(local_path_, &is_dir);
            LOG_STATUS_ERROR(
                    DeletePath(is_dir ? local_path_ : DirName(local_path_)),
                    "failed to delete localized path");
        }


        // Return the localized path represented by this object.
        const std::string &Path() const {
            return (local_path_.empty()) ? original_path_ : local_path_;
        }

        // Maintain a vector of LocalizedPath that should be kept available in the
        // tmp directory for the lifetime of this object
        // FIXME: Remove when no longer required
        std::vector <std::shared_ptr<LocalizedPath>> other_localized_path;
    }
private:
    std::string original_path_;
    std::string local_path_;
};

Status
CleanPath(const std::string& s3_path, std::string* clean_path)
{
    // Must handle paths with s3 prefix
    size_t start = s3_path.find("s3://");
    std::string path = "";
    // 没找到s3
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
        return Status(
                Status::Code::INVALID_ARG, "Invalid bucket name: '" + path + "'");
    }

    // Remove leading slashes
    size_t ltrim_length = path.find_first_not_of('/');
    if (ltrim_length == std::string::npos) {
        return Status(
                Status::Code::INVALID_ARG, "Invalid bucket name: '" + path + "'");
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

    return Status::Success;
}

Status
ParsePath(const std::string& path, std::string* bucket, std::string* object) {
    // Cleanup extra slashes
    std::string clean_path;
    RETURN_IF_ERROR(CleanPath(path, &clean_path));

    // Get the bucket name and the object path. Return error if path is malformed
    std::string protocol, host_name, host_port;
    if (!RE2::FullMatch(
            clean_path, s3_regex_, &protocol, &host_name, &host_port, bucket,object)) {
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
        return Status(
                Status::Code::INTERNAL, "No bucket name found in path: " + path);
    }

    return Status::Success;
}

Status
FileExists(const std::string& path, bool* exists) {
    *exists = false;

    // S3 doesn't make objects for directories, so it could still be a directory
    bool is_dir;
    RETURN_IF_ERROR(IsDirectory(path, &is_dir));
    if (is_dir) {
        *exists = is_dir;
        return Status::Success;
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
            return Status(
                    Status::Code::INTERNAL,
                    "Could not get MetaData for object at " + path +
                    " due to exception: " +
                    head_object_outcome.GetError().GetExceptionName() +
                    ", error message: " +
                    head_object_outcome.GetError().GetMessage());
        }
    } else {
        *exists = true;
    }

    return Status::Success;
}

Status
LocalizePath(const std::string& path, std::shared_ptr<LocalizedPath>* localized) {
    // Check if the directory or file exists
    bool exists;
    RETURN_IF_ERROR(FileExists(path, &exists));
    if (!exists) {
        return Status(
                Status::Code::INTERNAL, "directory or file does not exist at " + path);
    }

    // Cleanup extra slashes
    std::string clean_path;
    RETURN_IF_ERROR(CleanPath(path, &clean_path));

    // Remove protocol and host name and port
    std::string effective_path, protocol, host_name, host_port, bucket, object;
    if (RE2::FullMatch(
            clean_path, s3_regex_, &protocol, &host_name, &host_port, &bucket,
            &object)) {
        effective_path = "s3://" + bucket + object;
    } else {
        effective_path = path;
    }

    // Create temporary directory
    std::string tmp_folder;
    RETURN_IF_ERROR(
            triton::core::MakeTemporaryDirectory(FileSystemType::LOCAL, &tmp_folder));

    /*
    // 获取可用于更新的本地地址
    const char* location_cstr;
    RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocationAcquire(
            agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, &location_cstr));
    std::string tmp_folder(location_cstr);
*/

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
        // Set localized path
        std::string filename = effective_path.substr(effective_path.find_last_of('/') + 1);
        localized->reset(
                new LocalizedPath(effective_path, JoinPath({tmp_folder, filename})));
        // Specify only the file to be downloaded
        contents.insert(effective_path);
    }

    // Download all specified contents and nested contents
  while (contents.size() != 0) {
    std::set<std::string> tmp_contents = contents;
    contents.clear();
    for (auto iter = tmp_contents.begin(); iter != tmp_contents.end(); ++iter) {
      std::string s3_fpath = *iter;
      std::string s3_removed_path = s3_fpath.substr(effective_path.size());
      std::string local_fpath =
          s3_removed_path.empty()
              ? (*localized)->Path()
              : JoinPath({(*localized)->Path(), s3_removed_path});
      bool is_subdir;
      RETURN_IF_ERROR(IsDirectory(s3_fpath, &is_subdir));
      if (is_subdir) {
          // Create local mirror of sub-directories
#ifdef _WIN32
        int status = mkdir(const_cast<char*>(local_fpath.c_str()));
#else
        int status = mkdir(
            const_cast<char*>(local_fpath.c_str()),
            S_IRUSR | S_IWUSR | S_IXUSR);
#endif
        if (status == -1) {
          return Status(
              Status::Code::INTERNAL,
              "Failed to create local folder: " + local_fpath +
                  ", errno:" + strerror(errno));
        }

        // Add sub-directories and deeper files to contents
        std::set<std::string> subdir_contents;
        RETURN_IF_ERROR(GetDirectoryContents(s3_fpath, &subdir_contents));
        for (auto itr = subdir_contents.begin(); itr != subdir_contents.end();
             ++itr) {
          contents.insert(JoinPath({s3_fpath, *itr}));
        }
      } else {
          // Create local copy of file
        std::string file_bucket, file_object;
        RETURN_IF_ERROR(ParsePath(s3_fpath, &file_bucket, &file_object));

        aws::string URL_aws;
        URL_aws = client_->GeneratePresignedUrl(
                aws_s(file_bucket.c_str(), file_bucket.size()), aws_s(file_object.c_str(), file_object.size()), HTTP_GET, expiresInSec);
        std::string URL(URL_aws.c_str(), URL_aws.size())


        httplib::Client cli("localhost", 65001);

        auto res = cli.Get(URL)
        if(res->status == 200) {
            auto& retrieved_file =
                    res->body;
            std::ofstream output_file(local_fpath.c_str(), std::ios::binary);
            output_file << retrieved_file.rdbuf();
            output_file.close
        } else {
            return Status(
                    Status::Code::INTERNAL,
                    "Failed to get object at " + s3_fpath + " due to exception: " +
                    res.error());
        }
      }
    }
  }

  return Status::Success;
}

bool
GetBucketAndObjectName(const std::string& s3Path, std::string& bucketName, std::string& objectName){
    std::string pathWithoutPrefix = s3Path.substr(5); 
    size_t delimiterPos = pathWithoutPrefix.find('/');

    if (delimiterPos != std::string::npos && delimiterPos != 0 && delimiterPos != pathWithoutPrefix.length() - 1)
    {
        bucketName = pathWithoutPrefix.substr(0, delimiterPos);
        objectName = pathWithoutPrefix.substr(delimiterPos + 1);
        return true;
    } else {
        bucketName = "";
        objectName = "";
        return false
    }
}
} // namespace

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

    // 获取模型远端初始地址
    const char* location_cstr;
    TRITONREPOAGENT_ArtifactType artifact_type;
    RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocation(
        agent, model, &artifact_type, &location_cstr));
    const std::string location(location_cstr);

    RETURN_IF_ERROR(LocalizePath(location))

    // 更新模型库
    RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryUpdate(
            agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, Local_path));
    }

    return nullptr;
}

}  // extern "C"

}}  // namespace triton::repoagent::dragonfly