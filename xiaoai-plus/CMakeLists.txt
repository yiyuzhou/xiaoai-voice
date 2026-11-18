cmake_minimum_required(VERSION 3.16)
project(xiaoai_plus LANGUAGES CXX)

option(XIAOAI_NATIVE_ONLY "Build the native speech frontend without ALSA capture/KWS" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SHERPA_ONNX_STATIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets/sherpa_onnx_static" CACHE PATH "Path to sherpa_onnx static package root")

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# Force ZLIB and OpenSSL to resolve to static .a BEFORE FetchContent
# so that ixwebsocket picks up the static versions.
find_library(ZLIB_STATIC_LIB NAMES libz.a HINTS ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)
find_library(SSL_STATIC_LIB NAMES libssl.a HINTS ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)
find_library(CRYPTO_STATIC_LIB NAMES libcrypto.a HINTS ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)
if(ZLIB_STATIC_LIB)
  set(ZLIB_LIBRARY ${ZLIB_STATIC_LIB} CACHE FILEPATH "" FORCE)
endif()


if(SSL_STATIC_LIB)
  set(OPENSSL_SSL_LIBRARY ${SSL_STATIC_LIB} CACHE FILEPATH "" FORCE)
endif()
if(CRYPTO_STATIC_LIB)
  set(OPENSSL_CRYPTO_LIBRARY ${CRYPTO_STATIC_LIB} CACHE FILEPATH "" FORCE)
endif()

FetchContent_Declare(
  nlohmann_json
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
  URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  ixwebsocket
  URL https://codeload.github.com/machinezone/IXWebSocket/tar.gz/refs/tags/v11.4.5
  URL_HASH SHA256=c5fc225edec32bf7d583e55347ef2c9c4940d005c13ef5e848354a85602f5fd6
)
set(USE_TLS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

FetchContent_Declare(
  spdlog
  URL https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.15.1
  URL_HASH SHA256=25c843860f039a1600f232c6eb9e01e6627f7d030a2ae5e232bdd3c9205d26cc
)
FetchContent_MakeAvailable(spdlog)

find_package(Threads REQUIRED)

if(XIAOAI_NATIVE_ONLY)
  add_executable(xiaoai_native_probe
    src/native_probe_main.cpp
    src/native_speech/probe.cpp
    src/native_speech/source.cpp)
  target_include_directories(xiaoai_native_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(xiaoai_native_probe PRIVATE Threads::Threads)
  if(CMAKE_CROSSCOMPILING)
    target_link_options(xiaoai_native_probe PRIVATE -static -Wl,-z,noexecstack)
  endif()
  add_executable(xiaoai_native_speaker
    src/native_main.cpp
    src/app/native_app.cpp
    src/native_speech/source.cpp
    src/audio/player_aplay.cpp
    src/config/config.cpp
    src/realtime/client.cpp
    src/realtime/daily_usage.cpp
    src/realtime/o2_protocol.cpp
    src/realtime/frame_codec.cpp
    src/realtime/duplex_protocol.cpp)
  target_include_directories(xiaoai_native_speaker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(xiaoai_native_speaker PRIVATE
    nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket spdlog::spdlog
    ${ZLIB_STATIC_LIB} ${SSL_STATIC_LIB} ${CRYPTO_STATIC_LIB}
    Threads::Threads ${CMAKE_DL_LIBS})
  if(CMAKE_CROSSCOMPILING)
    target_link_options(xiaoai_native_speaker PRIVATE -static -Wl,-z,noexecstack)
  endif()
  return()
endif()

# --- sherpa-onnx static libraries ---
find_path(SHERPA_ONNX_INCLUDE_DIR
  NAMES sherpa-onnx/c-api/cxx-api.h
  HINTS ${SHERPA_ONNX_STATIC_ROOT}
  PATH_SUFFIXES include
  NO_CMAKE_FIND_ROOT_PATH)

set(_sherpa_static_libs
  sherpa-onnx-cxx-api
  sherpa-onnx-c-api
  sherpa-onnx-core
  kaldi-decoder-core
  sherpa-onnx-kaldifst-core
  sherpa-onnx-fstfar
  sherpa-onnx-fst
  kaldi-native-fbank-core
  kissfft-float
  onnxruntime
  ssentencepiece_core
)
set(SHERPA_ONNX_STATIC_LIBRARIES "")
foreach(_lib ${_sherpa_static_libs})
  find_library(_found_${_lib}
    NAMES ${_lib}
    HINTS ${SHERPA_ONNX_STATIC_ROOT}
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)
  if(NOT _found_${_lib})
    message(FATAL_ERROR "Static lib not found: ${_lib} in ${SHERPA_ONNX_STATIC_ROOT}/lib")
  endif()
  list(APPEND SHERPA_ONNX_STATIC_LIBRARIES ${_found_${_lib}})
endforeach()

# --- webrtc static library ---
find_library(WEBRTC_STATIC_LIB NAMES libwebrtc_audio_processing.a HINTS ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)

if(NOT SHERPA_ONNX_INCLUDE_DIR)
  message(FATAL_ERROR "sherpa-onnx headers not found. Set SHERPA_ONNX_STATIC_ROOT")
endif()
if(NOT ZLIB_STATIC_LIB)
  message(FATAL_ERROR "libz.a not found in sysroot")
endif()
if(NOT WEBRTC_STATIC_LIB)
  message(FATAL_ERROR "libwebrtc_audio_processing.a not found in sysroot")
endif()

find_path(ZLIB_INCLUDE_DIR NAMES zlib.h HINTS ${CMAKE_SYSROOT}/usr/include)
find_path(WEBRTC_INCLUDE_DIR NAMES webrtc/modules/audio_processing/include/audio_processing.h
  HINTS ${CMAKE_SYSROOT}/usr/include/webrtc_audio_processing ${CMAKE_SYSROOT}/usr/include)

# --- core library ---
add_library(xiaoai_plus_core
  src/memory/core.cpp
  src/memory/service.cpp
  src/memory/http.cpp
  src/memory/selftest.cpp
  src/app/app.cpp
  src/input/stop_key.cpp
  src/app/native_wake_monitor.cpp
  src/audio/recorder_arecord.cpp
  src/audio/kws_diagnostics.cpp
  src/audio/kws_preprocess.cpp
  src/audio/player_aplay.cpp
  src/config/config.cpp
  src/audio/aec_webrtc.cpp
  src/audio/shadow_voice.cpp
  src/audio/shadow_webrtc.cpp
  src/audio/ordinary_barge_capture.cpp
  src/realtime/client.cpp
  src/realtime/daily_usage.cpp
  src/realtime/o2_protocol.cpp
  src/realtime/frame_codec.cpp
  src/realtime/duplex_protocol.cpp
  src/wakeup/gate.cpp
  src/wakeup/trigger.cpp
  src/wakeup/dual_wake_decider.cpp
  src/wakeup/local_listener.cpp
  src/wakeup/kws_zipformer.cpp
)

target_include_directories(xiaoai_plus_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${SHERPA_ONNX_INCLUDE_DIR}
  ${ZLIB_INCLUDE_DIR}
)
if(WEBRTC_INCLUDE_DIR)
  target_include_directories(xiaoai_plus_core PUBLIC ${WEBRTC_INCLUDE_DIR})
endif()
target_compile_definitions(xiaoai_plus_core PUBLIC WEBRTC_POSIX WEBRTC_AUDIO_PROCESSING_ONLY_BUILD)

target_link_libraries(xiaoai_plus_core
  PUBLIC
    nlohmann_json::nlohmann_json
    ixwebsocket::ixwebsocket
    spdlog::spdlog
    ${SHERPA_ONNX_STATIC_LIBRARIES}
    ${WEBRTC_STATIC_LIB}
    ${ZLIB_STATIC_LIB}
    ${SSL_STATIC_LIB}
    ${CRYPTO_STATIC_LIB}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# Read-only input mapping utility; only run during the later authorized install.
add_executable(xiaoai_input_keys toolchain/input-keys.cpp)
if(CMAKE_CROSSCOMPILING)
  target_link_options(xiaoai_input_keys PRIVATE -static -Wl,-z,noexecstack)
endif()

# --- main executable ---
add_executable(xiaoai_plus_speaker src/main.cpp)
target_link_libraries(xiaoai_plus_speaker PRIVATE xiaoai_plus_core)

# Fully static binary so it runs on older glibc devices (device has glibc 2.25)
if(CMAKE_CROSSCOMPILING)
  target_link_options(xiaoai_plus_speaker PRIVATE -static -Wl,-z,noexecstack)
endif()

# Local integration probe; never part of the resident binary or deployment.
add_executable(ordinary_barge_app_offline_test EXCLUDE_FROM_ALL tests/ordinary_barge_app_offline_test.cpp)
target_link_libraries(ordinary_barge_app_offline_test PRIVATE xiaoai_plus_core)
target_compile_options(ordinary_barge_app_offline_test PRIVATE -UNDEBUG)
if(CMAKE_CROSSCOMPILING)
  target_link_options(ordinary_barge_app_offline_test PRIVATE -static -Wl,-z,noexecstack)
endif()

foreach(probe stop_key_app_test ordinary_barge_identity_app_test resume_snapshot_lifecycle_test
              kws_consumer_offline_test kws_prefix_freshness_test)
  add_executable(${probe} EXCLUDE_FROM_ALL tests/${probe}.cpp)
  target_link_libraries(${probe} PRIVATE xiaoai_plus_core)
  target_compile_options(${probe} PRIVATE -UNDEBUG)
  if(CMAKE_CROSSCOMPILING)
    target_link_options(${probe} PRIVATE -static -Wl,-z,noexecstack)
  endif()
endforeach()
