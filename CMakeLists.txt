cmake_minimum_required(VERSION 3.24)

project(SpatialAudioLab LANGUAGES CXX)

include(CTest)

add_executable(SpatialAudioLab.CLI
  src/audio_platform.cpp
  src/bridge_meter.cpp
  src/capture_commands.cpp
  src/crossover_stage.cpp
  src/dolby_probe.cpp
  src/dtsx_analysis.cpp
  src/layout_mix.cpp
  src/media_foundation_probe.cpp
  src/mat_analysis.cpp
  src/mat_capture_client.cpp
  src/mat_format.cpp
  src/mat_pipeline.cpp
  src/multi_endpoint_renderer.cpp
  src/nsp_ring_reader.cpp
  src/omniphony_pcm_bridge.cpp
  src/pcm_pipeline.cpp
  src/speaker_layout.cpp
  src/spatial_audio_sample.cpp
  src/wave_io.cpp
)
target_compile_features(SpatialAudioLab.CLI PRIVATE cxx_std_20)
target_compile_definitions(SpatialAudioLab.CLI PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# shell32: SHGetKnownFolderPath, used to find the crossover config without
# trusting environment variables.
target_link_libraries(SpatialAudioLab.CLI PRIVATE ole32 shell32 runtimeobject windowsapp uuid avrt propsys mmdevapi mf mfplat mfreadwrite mfuuid)

if(MSVC)
  target_compile_options(SpatialAudioLab.CLI PRIVATE /W4 /permissive- /EHsc)
endif()

# Standalone MAT (Dolby MAT / TrueHD) recorder CLI. Self-contained: it only
# depends on the driver contract header (matcapture.h) and the Windows SDK, so it
# does not reuse the larger SpatialAudioLab.CLI translation units.
add_executable(SpatialAudioLab.MatRecorder
  src/mat_recorder_cli.cpp
)
target_compile_features(SpatialAudioLab.MatRecorder PRIVATE cxx_std_20)
target_compile_definitions(SpatialAudioLab.MatRecorder PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SpatialAudioLab.MatRecorder PRIVATE
  ole32 shell32 runtimeobject windowsapp uuid avrt propsys mmdevapi)
if(MSVC)
  target_compile_options(SpatialAudioLab.MatRecorder PRIVATE /W4 /permissive- /EHsc)
endif()

if(BUILD_TESTING)
  add_executable(SpatialAudioLab.NativeTests
    tests/native/layout_mix_tests.cpp
    src/audio_platform.cpp
    src/layout_mix.cpp
    src/speaker_layout.cpp
  )
  target_compile_features(SpatialAudioLab.NativeTests PRIVATE cxx_std_20)
  target_compile_definitions(SpatialAudioLab.NativeTests PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_include_directories(SpatialAudioLab.NativeTests PRIVATE src)
  target_link_libraries(SpatialAudioLab.NativeTests PRIVATE
    ole32 runtimeobject windowsapp uuid avrt propsys mmdevapi)
  if(MSVC)
    target_compile_options(SpatialAudioLab.NativeTests PRIVATE /W4 /permissive- /EHsc)
  endif()
  add_test(NAME SpatialAudioLab.NativeTests COMMAND SpatialAudioLab.NativeTests)
endif()
