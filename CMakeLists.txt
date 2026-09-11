# Copyright (c) 2026 PacketFlow (packetflow.dev)
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

project(vpp-rtp-asr-plugin)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(VPP REQUIRED)
find_package(PkgConfig REQUIRED)

# Sherpa-ONNX (Moonshine / Parakeet / Zipformer / Whisper runtime)
pkg_check_modules(SHERPA_ONNX REQUIRED sherpa-onnx)

# FFmpeg: codec decode + resample
pkg_check_modules(AV REQUIRED libavcodec libavutil libswresample)

# libopus: direct Opus decode (lower overhead than libavcodec wrapper for Opus)
pkg_check_modules(OPUS REQUIRED opus)

# Optional: CUDA execution provider in ONNX Runtime
option(WITH_CUDA "Enable CUDA execution provider for Sherpa-ONNX" OFF)

# Optional: TensorRT in-process inference backend (v2 scope)
option(WITH_TENSORRT "Enable TensorRT backend (v2 scope — not implemented)" OFF)
if(WITH_TENSORRT)
    message(FATAL_ERROR "WITH_TENSORRT is a v2 scope — not implemented in this tree yet.")
endif()

include_directories(${CMAKE_SOURCE_DIR}/src/plugins)
include_directories(${SHERPA_ONNX_INCLUDE_DIRS} ${AV_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS})
link_directories(${SHERPA_ONNX_LIBRARY_DIRS} ${AV_LIBRARY_DIRS} ${OPUS_LIBRARY_DIRS})

add_subdirectory(src/plugins/rtp_asr)
