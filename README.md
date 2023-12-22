# dragonfly-repository-agent

[![CI](https://github.com/dragonflyoss/dragonfly-repository-agent/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/dragonflyoss/dragonfly-repository-agent/actions/workflows/ci.yml)
[![LICENSE](https://img.shields.io/github/license/dragonflyoss/Dragonfly2.svg?style=flat-square)](https://github.com/dragonflyoss/Dragonfly2/blob/main/LICENSE)

The Triton repository agent that downloads model via dragonfly.



# Example

### 1.拉取镜像：
    
    docker pull dragonflyoss/dragonfly-repository-agent:latest


### 2.设置配置文件：
2.1:在config.pbtxt远程模型仓库配置文件中添加以下字段：

     model_repository_agents{

      agents [
          {
               name: "dragonfly",
          }
      ] 
    }

2.2：在cloud_credential.json文件中添加认证信息:

    {         
     "s3": {
       "": {
       "secret_key":"",
       "key_id":"",
       "region": "",
       "session_token":"",
       "profile": ""
        }
      } //s3认证信息
     "gs":{}
     "as":{}  
    }

2.3：在dragonfly_config.json文件中添加认证信息:

    {
     "proxy": "http://127.0.0.1:65001", // Dragonfly 的地址
     "header": {},
     "filter": [
     "X-Amz-Algorithm",
     "X-Amz-Credential&X-Amz-Date",
     "X-Amz-Expires",
     "X-Amz-SignedHeaders",
     "X-Amz-Signature"]
    }
  
### 3.执行命令：

    sudo docker run  -it  --network host  -v {配置文件路径}:/home/triton/ -e TRITON_DRAGONFLY_CONFIG_PATH=/home/triton/dragonfly_config.json -e  TRITON_CLOUD_CREDENTIAL_PATH=/home/triton/cloud_credential.json  dragonflyoss/dragonfly-repository-agent  tritonserver --model-repository=s3://{ip}/models  --exit-on-error=false

### 4.查看输出结果：
       
      I1213 12:27:11.166347 1 server.cc:604]  
      +------------------+------------------------------------------------------------------------+
      | Repository Agent | Path                                                                   |
      +------------------+------------------------------------------------------------------------+
      | dragonfly        | /opt/tritonserver/repoagents/dragonfly/libtritonrepoagent_dragonfly.so |
      +------------------+------------------------------------------------------------------------+

      +--------+---------+--------+
      | Model  | Version | Status |
      +--------+---------+--------+
      | simple | 1       | READY  |
      +--------+---------+--------+

   




## Documentation

You can find the full documentation on the [d7y.io](https://d7y.io).

## Community

Join the conversation and help the community.

- **Slack Channel**: [#dragonfly](https://cloud-native.slack.com/messages/dragonfly/) on [CNCF Slack](https://slack.cncf.io/)
- **Discussion Group**: <dragonfly-discuss@googlegroups.com>
- **Developer Group**: <dragonfly-developers@googlegroups.com>
- **Github Discussions**: [Dragonfly Discussion Forum](https://github.com/dragonflyoss/Dragonfly2/discussions)
- **Twitter**: [@dragonfly_oss](https://twitter.com/dragonfly_oss)
- **DingTalk**: [22880028764](https://qr.dingtalk.com/action/joingroup?code=v1,k1,pkV9IbsSyDusFQdByPSK3HfCG61ZCLeb8b/lpQ3uUqI=&_dt_no_comment=1&origin=11)

## Contributing

You should check out our
[CONTRIBUTING](./CONTRIBUTING.md) and develop the project together.
