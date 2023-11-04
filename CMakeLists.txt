# Copyright (c) 2021-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

cmake_minimum_required(VERSION 3.17)

project(tritondragonflyrepoagent LANGUAGES C CXX)

#
# Options
#
# Must include options required for this project as well as any
# projects included in this one by FetchContent.
#

set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "")
set(CMAKE_TOOLCHAIN_FILE /Users/chenyufei/code/cpp/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "")

set(TRITON_COMMON_REPO_TAG "main" CACHE STRING "Tag for triton-inference-server/common repo")
set(TRITON_CORE_REPO_TAG "main" CACHE STRING "Tag for triton-inference-server/core repo")

#
# Dependencies
#
# FetchContent's composability isn't very good. We must include the
# transitive closure of all repos so that we can override the tag.
#
include(FetchContent)

FetchContent_Declare(
        repo-common
        GIT_REPOSITORY https://github.com/triton-inference-server/common.git
        GIT_TAG ${TRITON_COMMON_REPO_TAG}
        GIT_SHALLOW ON
)
FetchContent_Declare(
        repo-core
        GIT_REPOSITORY https://github.com/triton-inference-server/core.git
        GIT_TAG ${TRITON_CORE_REPO_TAG}
        GIT_SHALLOW ON
)

FetchContent_MakeAvailable(repo-common repo-core)

configure_file(src/libtritonrepoagent_dragonfly.ldscript libtritonrepoagent_dragonfly.ldscript COPYONLY)

add_library(
        triton-dragonfly-repoagent SHARED
        src/dragonfly.cpp
        src/filesystem/api.cpp
        src/filesystem/api.h
        src/status.h
        src/filesystem/implementations/common.h
        src/filesystem/implementations/s3.h
        src/filesystem/implementations/gcs.h
        src/filesystem/implementations/as.h
        src/config.h
        src/config.h
        src/common_utils.h
        src/common_utils.h
)

add_library(
        TritonDragonflyRepoAgent::triton-dragonfly-repoagent ALIAS triton-dragonfly-repoagent
)

target_include_directories(
        triton-dragonfly-repoagent
        PUBLIC
        $<INSTALL_INTERFACE:include>
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(re2 CONFIG REQUIRED)
target_link_libraries(triton-dragonfly-repoagent PRIVATE re2::re2)
#
# S3
#
if(${TRITON_ENABLE_S3})
    find_package(ZLIB REQUIRED)
    find_package(AWSSDK REQUIRED COMPONENTS core s3)
    message(STATUS "Using aws-sdk-cpp ${AWSSDK_VERSION}")
    target_include_directories(
            triton-dragonfly-repoagent
            PRIVATE
            $<TARGET_PROPERTY:aws-cpp-sdk-core,INTERFACE_INCLUDE_DIRECTORIES>
            $<TARGET_PROPERTY:aws-cpp-sdk-s3,INTERFACE_INCLUDE_DIRECTORIES>
    )

    target_link_libraries(
            triton-dragonfly-repoagent
            PRIVATE
            aws-cpp-sdk-s3 aws-cpp-sdk-core
    )
endif() # TRITON_ENABLE_S3

if(${TRITON_ENABLE_GCS})
    find_package(google_cloud_cpp_storage REQUIRED)
    message(STATUS "Using google-cloud-cpp ${google_cloud_cpp_storage_VERSION}")
    target_include_directories(
            triton-dragonfly-repoagent
            PRIVATE $<TARGET_PROPERTY:google-cloud-cpp::storage,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(
            triton-dragonfly-repoagent
            PRIVATE
            google-cloud-cpp::storage
    )
endif() # TRITON_ENABLE_GCS

if(${TRITON_ENABLE_AZURE_STORAGE})
    find_package(azure-storage-blobs-cpp CONFIG REQUIRED)
    message(STATUS "Using Azure storage blobs ${azure-storage-blobs-cpp_VERSION}")
    target_include_directories(
            triton-dragonfly-repoagent
            PRIVATE $<TARGET_PROPERTY:Azure::azure-storage-blobs,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_link_libraries(
            triton-dragonfly-repoagent
            PRIVATE
            Azure::azure-storage-blobs
    )
endif()

target_compile_features(triton-dragonfly-repoagent PRIVATE cxx_std_11)
target_compile_options(
        triton-dragonfly-repoagent PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wno-unused-parameter -Wno-type-limits -Werror>
)

target_link_libraries(
        triton-dragonfly-repoagent
        PRIVATE
        triton-core-serverapi   # from repo-core
        triton-core-backendapi
        triton-core-repoagentapi   # from repo-core
        triton-core-serverstub  # from repo-core
        triton-common-error #from repo-common
)


set_target_properties(
        triton-dragonfly-repoagent PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME tritonrepoagent_dragonfly
        LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libtritonrepoagent_dragonfly.ldscript
        LINK_FLAGS "-Wl,--version-script libtritonrepoagent_dragonfly.ldscript"
)

include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/TritonDragonflyRepoAgent)

install(
        TARGETS
        triton-dragonfly-repoagent
        EXPORT
        triton-dragonfly-repoagent-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/repoagents/dragonfly
        ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/repoagents/dragonfly
)

install(
        EXPORT
        triton-dragonfly-repoagent-targets
        FILE
        TritonDragonflyRepoAgentTargets.cmake
        NAMESPACE
        TritonDragonflyRepoAgent::
        DESTINATION
        ${INSTALL_CONFIGDIR}
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
        ${CMAKE_CURRENT_LIST_DIR}/cmake/TritonDragonflyRepoAgentConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/TritonDragonflyRepoAgentConfig.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
)

install(
        FILES
        ${CMAKE_CURRENT_BINARY_DIR}/TritonDragonflyRepoAgentConfig.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
)

export(
        EXPORT triton-dragonfly-repoagent-targets
        FILE ${CMAKE_CURRENT_BINARY_DIR}/TritonDragonflyRepoAgentTargets.cmake
        NAMESPACE TritonDragonflyRepoAgent::
)

export(PACKAGE TritonDragonflyRepoAgent)