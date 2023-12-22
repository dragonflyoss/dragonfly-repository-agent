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

#include <string>

#include "filesystem/api.h"
#include "triton/core/tritonrepoagent.h"

namespace triton::repoagent::dragonfly {

/////////////

extern "C" {

TRITONSERVER_Error*
TRITONREPOAGENT_ModelAction(
    TRITONREPOAGENT_Agent* agent, TRITONREPOAGENT_AgentModel* model,
    const TRITONREPOAGENT_ActionType action_type)
{
  switch (action_type) {
    case TRITONREPOAGENT_ACTION_LOAD: {
      const char* location_cstr;
      TRITONREPOAGENT_ArtifactType artifact_type;
      RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocation(
          agent, model, &artifact_type, &location_cstr));
      const std::string location(location_cstr);

      const char* file_path_c_str = std::getenv("TRITON_CLOUD_CREDENTIAL_PATH");
      std::string cred_path;
      if (file_path_c_str != nullptr) {
        // Load from credential file
        cred_path = std::string(file_path_c_str);
      }else{
        cred_path="/home/triton/cloud_credential.json" ;
      }

      const char* config_path_c_str =
          std::getenv("TRITON_DRAGONFLY_CONFIG_PATH");
      std::string config_path;
      if (config_path_c_str != nullptr) {
        // Load from config file
        config_path = std::string(config_path_c_str);
      }else{
        config_path="/home/triton/dragonfly_config.json" ;
      }

      const char* temp_dir_cstr = nullptr;
      RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryLocationAcquire(
          agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, &temp_dir_cstr));
      const std::string temp_dir(temp_dir_cstr);

      try {
        RETURN_IF_ERROR(
            LocalizePath(config_path, cred_path, location, temp_dir));

        char* l = const_cast<char*>(temp_dir.c_str());
        RETURN_IF_ERROR(TRITONREPOAGENT_ModelRepositoryUpdate(
            agent, model, TRITONREPOAGENT_ARTIFACT_FILESYSTEM, l));
      }
      catch (const ErrorException& ee) {
        return ee.err_;
      }

      return nullptr;  // success
    }
    default:
      return nullptr;
  }

}  // extern "C"
}
}  // namespace triton::repoagent::dragonfly
