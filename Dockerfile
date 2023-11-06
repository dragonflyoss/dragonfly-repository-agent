# Use a base image that includes development essentials and git
FROM ubuntu:latest AS builder

# Install build tools, git, and other dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    zip \
    unzip \
    tar \
    pkg-config \
    python3

# Install vcpkg
RUN git clone https://github.com/Microsoft/vcpkg.git /vcpkg \
    && /vcpkg/bootstrap-vcpkg.sh

# Install the required dependencies using vcpkg
RUN /vcpkg/vcpkg install re2 \
    && /vcpkg/vcpkg install aws-sdk-cpp \
    && /vcpkg/vcpkg install google-cloud-cpp[storage] \
    && /vcpkg/vcpkg install azure-storage-blobs-cpp \
    && /vcpkg/vcpkg install rapidjson

RUN mkdir -p /dragonfly-repository-agent/build

# Assuming the source code is located in a directory called 'source'
# You can COPY this in or clone from a repository as needed
COPY ./src /dragonfly-repository-agent/src
COPY ./cmake /dragonfly-repository-agent/cmake
COPY ./CMakeLists.txt /dragonfly-repository-agent/CMakeLists.txt

# Set the working directory
WORKDIR /dragonfly-repository-agent/build

# Set the environment variable for the vcpkg toolchain file
ENV VCPKG_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build the repository agent
RUN cmake -DCMAKE_TOOLCHAIN_FILE=${VCPKG_TOOLCHAIN_FILE} \
          -DCMAKE_INSTALL_PREFIX:PATH=`pwd`/install \
          -DTRITON_ENABLE_GCS=true \
          -DTRITON_ENABLE_AZURE_STORAGE=true \
          -DTRITON_ENABLE_S3=true \
          -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON \
          ..

RUN make install

FROM nvcr.io/nvidia/tritonserver:23.08-py3

RUN mkdir /opt/tritonserver/repoagents/dragonfly

COPY --from=builder /dragonfly-repository-agent/build/libtritonrepoagent_dragonfly.so /opt/tritonserver/repoagents/dragonfly/libtritonrepoagent_dragonfly.so
