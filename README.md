# dragonfly-repository-agent
The Triton repository agent that downloads model via dragonfly.

## Build the Dragonfly Repository Agent
Use a recent cmake to build. First install the required dependencies by vcpkg.
```

$ vcpkg install re2
```
Install the required dependencies for object storage.
```
$ vcpkg install aws-sdk-cpp
$ vcpkg install google-cloud-cpp[storage]
$ vcpkg install azure-storage-blobs-cpp
```
To build the repository agent:
```
$ mkdir build
$ cd build
$ cmake -D CMAKE_INSTALL_PREFIX:PATH=`pwd`/install -D TRITON_ENABLE_GCS=true -D TRITON_ENABLE_AZURE_STORAGE=true -D TRITON_ENABLE_S3=true ..
$ make install
```

## Using the Dragonfly Repository Agent
The dragonfly repository agent is configured by plugin name in the ModelRepositoryAgents section of the model configuration.
```
model_repository_agents
{
  agents [
    {
      name: "dragonfly",
    }
  ]
}

```
To group the credentials into a single file for Triton, you may set the `TRITON_CLOUD_CREDENTIAL_PATH` environment variable to a path pointing to a JSON file of the following format, residing in the local file system.
```
export TRITON_CLOUD_CREDENTIAL_PATH="cloud_credential.json"
```

"cloud_credential.json":
```
{
  "gs": {
    "": "PATH_TO_GOOGLE_APPLICATION_CREDENTIALS",
    "gs://gcs-bucket-002": "PATH_TO_GOOGLE_APPLICATION_CREDENTIALS_2"
  },
  "s3": {
    "": {
      "secret_key": "AWS_SECRET_ACCESS_KEY",
      "key_id": "AWS_ACCESS_KEY_ID",
      "region": "AWS_DEFAULT_REGION",
      "session_token": "",
      "profile": ""
    },
    "s3://s3-bucket-002": {
      "secret_key": "AWS_SECRET_ACCESS_KEY_2",
      "key_id": "AWS_ACCESS_KEY_ID_2",
      "region": "AWS_DEFAULT_REGION_2",
      "session_token": "AWS_SESSION_TOKEN_2",
      "profile": "AWS_PROFILE_2"
    }
  },
  "as": {
    "": {
      "account_str": "AZURE_STORAGE_ACCOUNT",
      "account_key": "AZURE_STORAGE_KEY"
    },
    "as://Account-002/Container": {
      "account_str": "",
      "account_key": ""
    }
  }
}
```

Set the configuration file for Dragonfly. You may set the `TRITON_DRAGONFLY_CONFIG_PATH` environment variable to a path pointing to a JSON file of the following format, residing in the local file system.
```
export TRITON_DRAGONFLY_CONFIG_PATH="dragonfly_config.json"
```

```
"dragonfly_config.json":
{
	"proxy": "http://127.0.0.1:65001",
	"header": {
	},
	"filter": [
		"X-Amz-Algorithm",
		"X-Amz-Credential&X-Amz-Date",
		"X-Amz-Expires",
		"X-Amz-SignedHeaders",
		"X-Amz-Signature"
	],
}
```
- proxy:  Address of Dragonfly's Peer Proxy.
- header: Add request headers to the request.
- filter: Used to generate a unique task and filter out unnecessary query parameters in the URL.