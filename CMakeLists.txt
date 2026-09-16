cmake_minimum_required(VERSION 3.8)
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

project(blah2)
option(OWL_GPU_FIR "Build selected Vulkan/VkFFT clutter FIR backend" OFF)
set(OWL_VKFFT_ROOT "" CACHE PATH "Pinned VkFFT 1.3.4 checkout")
include(CMakePrintHelpers)
include(CTest)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -O3 -march=armv8-a+simd")

find_package(Threads REQUIRED)
find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")
find_package(asio  REQUIRED)
find_package(ryml CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(Armadillo CONFIG REQUIRED)
find_package(Catch2 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(IIO REQUIRED libiio)



set(PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_ROOT}/bin")
set(PROJECT_BINARY_TEST_DIR "${PROJECT_ROOT}/bin/test")
set(PROJECT_BINARY_TEST_UNIT_DIR "${PROJECT_BINARY_TEST_DIR}/unit")
set(PROJECT_BINARY_TEST_FUNCTIONAL_DIR "${PROJECT_BINARY_TEST_DIR}/functional")
set(PROJECT_BINARY_TEST_COMPARISON_DIR "${PROJECT_BINARY_TEST_DIR}/comparison")

message("Binary path: ${PROJECT_BINARY_DIR}")
message("Binary test path: ${PROJECT_BINARY_TEST_DIR}")

# include from top-level src dir
include_directories(src ${IIO_INCLUDE_DIRS})

# TODO: create FindSdrplay.cmake for this
add_library(sdrplay /usr/local/include/sdrplay_api.h)
set_target_properties(sdrplay PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(sdrplay PUBLIC /usr/local/lib/libsdrplay_api.so.3.15)

# TODO: Move to separate src/CMakeLists.txt
add_executable(blah2
  src/blah2.cpp
  src/capture/Capture.cpp
  src/capture/Source.cpp
  src/capture/rspduo/RspDuo.cpp
  src/process/ambiguity/Ambiguity.cpp
  src/process/clutter/WienerHopf.cpp
  src/process/detection/CfarDetector1D.cpp
  src/process/detection/Centroid.cpp
  src/process/detection/Interpolate.cpp
  src/process/tracker/Tracker.cpp
  src/process/spectrum/SpectrumAnalyser.cpp
  src/process/meta/HammingNumber.cpp
  src/process/utility/Socket.cpp
  src/data/IqData.cpp
  src/data/Map.cpp
  src/data/Detection.cpp
  src/data/Track.cpp
  src/data/meta/Timing.cpp
)

target_link_libraries(blah2 PRIVATE
  Threads::Threads
  asio::asio
  ryml::ryml
  httplib::httplib
  armadillo
  ${IIO_LIBRARIES}
  fftw3
  fftw3_threads
  sdrplay
)
target_include_directories(blah2 PRIVATE RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")

if(OWL_GPU_FIR)
  if(NOT EXISTS "${OWL_VKFFT_ROOT}/vkFFT/vkFFT.h")
    message(FATAL_ERROR "OWL_GPU_FIR needs OWL_VKFFT_ROOT with vkFFT/vkFFT.h")
  endif()
  execute_process(COMMAND git -C "${OWL_VKFFT_ROOT}" rev-parse HEAD
    OUTPUT_VARIABLE OWL_VKFFT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE OWL_VKFFT_GIT_RESULT)
  if(NOT OWL_VKFFT_GIT_RESULT EQUAL 0 OR
     NOT OWL_VKFFT_COMMIT STREQUAL "066a17c17068c0f11c9298d848c2976c71fad1c1")
    message(FATAL_ERROR "OWL_GPU_FIR requires selected VkFFT 1.3.4 commit 066a17c17068c0f11c9298d848c2976c71fad1c1")
  endif()
  execute_process(COMMAND git -C "${OWL_VKFFT_ROOT}" status --porcelain
    OUTPUT_VARIABLE OWL_VKFFT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(OWL_VKFFT_DIRTY)
    message(FATAL_ERROR "OWL_GPU_FIR requires a clean VkFFT source checkout")
  endif()
  find_package(Vulkan REQUIRED)
  find_package(glslang CONFIG QUIET)
  if(NOT TARGET glslang::glslang)
    find_path(OWL_GLSLANG_LEGACY_CMAKE glslangTargets.cmake
      PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake
            /usr/lib64/cmake /usr/lib/cmake)
    if(OWL_GLSLANG_LEGACY_CMAKE)
      foreach(lib IN ITEMS OSDependent OGLCompiler HLSL glslang SPIRV
          glslang-default-resource-limits)
        if(EXISTS "${OWL_GLSLANG_LEGACY_CMAKE}/${lib}Targets.cmake")
          include("${OWL_GLSLANG_LEGACY_CMAKE}/${lib}Targets.cmake")
          if(TARGET ${lib} AND NOT TARGET glslang::${lib})
            add_library(glslang::${lib} ALIAS ${lib})
          endif()
        endif()
      endforeach()
    endif()
  endif()
  if(NOT TARGET glslang::glslang OR NOT TARGET glslang::SPIRV OR
     NOT TARGET glslang::glslang-default-resource-limits)
    message(FATAL_ERROR "OWL_GPU_FIR needs glslang development libraries")
  endif()
  find_path(OWL_GLSLANG_C_INCLUDE glslang_c_interface.h
    PATH_SUFFIXES glslang/Include)
  if(NOT OWL_GLSLANG_C_INCLUDE)
    message(FATAL_ERROR "OWL_GPU_FIR needs glslang C headers")
  endif()
  target_sources(blah2 PRIVATE src/process/clutter/gpu/OwlGpuFilter.cpp)
  target_compile_features(blah2 PRIVATE cxx_std_17)
  target_compile_definitions(blah2 PRIVATE OWL_ENABLE_GPU_FIR=1 VKFFT_BACKEND=0)
  target_compile_options(blah2 PRIVATE -Wno-missing-field-initializers)
  target_include_directories(blah2 SYSTEM PRIVATE
    "${OWL_VKFFT_ROOT}/vkFFT" "${OWL_GLSLANG_C_INCLUDE}")
  target_link_libraries(blah2 PRIVATE Vulkan::Vulkan glslang::glslang
    glslang::SPIRV glslang::glslang-default-resource-limits)
endif()

# unit tests
add_executable(testAmbiguity
  test/unit/process/ambiguity/TestAmbiguity.cpp
  src/data/IqData.cpp
  src/data/Map.cpp
  src/process/ambiguity/Ambiguity.cpp
  src/process/meta/HammingNumber.cpp
)
target_link_libraries(testAmbiguity PRIVATE
  Threads::Threads
  Catch2::Catch2WithMain
  fftw3
  fftw3_threads
)
set_target_properties(testAmbiguity PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testTracker
  test/unit/process/tracker/TestTracker.cpp
  src/data/Detection.cpp
  src/data/Track.cpp
  src/process/tracker/Tracker.cpp
)
target_link_libraries(testTracker PRIVATE
  Catch2::Catch2WithMain
)
set_target_properties(testTracker PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testHammingNumber
  test/unit/process/meta/TestHammingNumber.cpp
  src/process/meta/HammingNumber.cpp
)
target_link_libraries(testHammingNumber PRIVATE
  Catch2::Catch2WithMain
)
set_target_properties(testHammingNumber PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testClutterFft
  test/unit/process/clutter/TestClutterFft.cpp
  src/process/clutter/WienerHopf.cpp
  src/data/IqData.cpp
)
target_compile_definitions(testClutterFft PRIVATE OWL_CLUTTER_TEST_PLAN_FAILURE)
target_link_libraries(testClutterFft PRIVATE
  Threads::Threads
  armadillo
  fftw3
  fftw3_threads
)
set_target_properties(testClutterFft PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testJsonKm
  test/unit/data/TestJsonKm.cpp
  src/data/Map.cpp
  src/data/Detection.cpp
)
# armadillo is linked only to pick up the vcpkg include path, which is
# where rapidjson lives; RAPIDJSON_INCLUDE_DIRS is NOTFOUND in this build.
target_link_libraries(testJsonKm PRIVATE
  armadillo
)
set_target_properties(testJsonKm PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testAmbiguityIndexing
  test/unit/process/ambiguity/TestAmbiguityIndexing.cpp
  src/data/Map.cpp
)
# armadillo is linked only to pick up the vcpkg include path, which is
# where rapidjson lives; RAPIDJSON_INCLUDE_DIRS is NOTFOUND in this build.
target_link_libraries(testAmbiguityIndexing PRIVATE
  Threads::Threads
  armadillo
)
set_target_properties(testAmbiguityIndexing PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testCfarHoist
  test/unit/process/detection/TestCfarHoist.cpp
  src/process/detection/CfarDetector1D.cpp
  src/data/Map.cpp
  src/data/Detection.cpp
)
# armadillo is linked only to pick up the vcpkg include path, which is
# where rapidjson lives; RAPIDJSON_INCLUDE_DIRS is NOTFOUND in this build.
target_link_libraries(testCfarHoist PRIVATE
  armadillo
)
set_target_properties(testCfarHoist PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testCpiPipeline
  test/unit/process/utility/TestCpiPipeline.cpp
  src/data/IqData.cpp
)
# armadillo is linked only to pick up the vcpkg include path, which is
# where rapidjson lives; RAPIDJSON_INCLUDE_DIRS is NOTFOUND in this build.
target_link_libraries(testCpiPipeline PRIVATE
  armadillo
)
set_target_properties(testCpiPipeline PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testStageOrder
  test/unit/process/clutter/TestStageOrder.cpp
  src/process/clutter/WienerHopf.cpp
  src/process/spectrum/SpectrumAnalyser.cpp
  src/data/IqData.cpp
)
target_link_libraries(testStageOrder PRIVATE
  Threads::Threads
  armadillo
  fftw3
  fftw3_threads
)
set_target_properties(testStageOrder PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testCorrelationWorker
  test/unit/process/clutter/TestCorrelationWorker.cpp
)
target_link_libraries(testCorrelationWorker PRIVATE
  Threads::Threads
  fftw3
  fftw3_threads
)
set_target_properties(testCorrelationWorker PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

# TODO: Unsure if will be using CTest.
add_test(NAME testAmbiguity COMMAND testAmbiguity)
add_test(NAME testTracker COMMAND testTracker)
add_test(NAME testHammingNumber COMMAND testHammingNumber)
add_test(NAME testClutterFft COMMAND testClutterFft)
add_test(NAME testJsonKm COMMAND testJsonKm)
add_test(NAME testAmbiguityIndexing COMMAND testAmbiguityIndexing)
add_test(NAME testCfarHoist COMMAND testCfarHoist)
add_test(NAME testCpiPipeline COMMAND testCpiPipeline)
add_test(NAME testStageOrder COMMAND testStageOrder)
add_test(NAME testCorrelationWorker COMMAND testCorrelationWorker)

add_executable(testUsbMode
  test/unit/capture/rspduo/TestUsbMode.cpp
)
set_target_properties(testUsbMode PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

# TODO: Unsure if will be using CTest.
add_test(NAME testUsbMode COMMAND testUsbMode)

add_executable(testAmbiguityPort test/unit/process/ambiguity/TestAmbiguityPort.cpp src/process/ambiguity/Ambiguity.cpp src/process/meta/HammingNumber.cpp src/data/IqData.cpp src/data/Map.cpp)
target_link_libraries(testAmbiguityPort PRIVATE Threads::Threads armadillo fftw3 fftw3_threads)
add_test(NAME testAmbiguityPort COMMAND testAmbiguityPort)
add_executable(testSpectrumFold test/unit/process/spectrum/TestSpectrumFold.cpp src/process/spectrum/SpectrumAnalyser.cpp src/data/IqData.cpp)
target_link_libraries(testSpectrumFold PRIVATE Threads::Threads armadillo fftw3 fftw3_threads)
add_test(NAME testSpectrumFold COMMAND testSpectrumFold)

add_executable(testIqAssignment test/unit/data/TestIqAssignment.cpp src/data/IqData.cpp)
target_link_libraries(testIqAssignment PRIVATE armadillo)
add_test(NAME testIqAssignment COMMAND testIqAssignment)

add_executable(testSocket test/unit/process/utility/TestSocket.cpp src/process/utility/Socket.cpp)
target_link_libraries(testSocket PRIVATE Threads::Threads asio::asio)
add_test(NAME testSocket COMMAND testSocket)

add_executable(testSdkSampleClock test/unit/capture/TestSdkSampleClock.cpp)
add_executable(testPairedCpiQueue test/unit/capture/TestPairedCpiQueue.cpp)
add_executable(testPipelineShutdown test/unit/capture/TestPipelineShutdown.cpp)
target_link_libraries(testPipelineShutdown PRIVATE Threads::Threads)
add_executable(testPairedCallbacks
  test/unit/capture/TestPairedCallbacks.cpp
  src/capture/rspduo/RspDuo.cpp
  src/capture/Source.cpp
  src/data/IqData.cpp
)
target_link_libraries(testPairedCallbacks PRIVATE Threads::Threads sdrplay armadillo)
target_link_libraries(testPairedCpiQueue PRIVATE Threads::Threads)
foreach(capture_test testSdkSampleClock testPairedCpiQueue testPipelineShutdown testPairedCallbacks)
  set_target_properties(${capture_test} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
endforeach()
add_test(NAME testSdkSampleClock COMMAND testSdkSampleClock)
add_test(NAME testPairedCpiQueue COMMAND testPairedCpiQueue)
add_test(NAME testPipelineShutdown COMMAND testPipelineShutdown)
set_tests_properties(testPipelineShutdown PROPERTIES TIMEOUT 5)
add_test(NAME testPairedCallbacks COMMAND testPairedCallbacks
  "${PROJECT_BINARY_DIR}/paired-callbacks-test.iq")

# Keep every unit executable in the same tree for local discovery. CTest is
# still the authoritative runner because some tests have registered arguments.
foreach(unit_test IN ITEMS
    testAmbiguity testTracker testHammingNumber testClutterFft testJsonKm
    testAmbiguityIndexing testCfarHoist testCpiPipeline testStageOrder
    testCorrelationWorker testUsbMode testAmbiguityPort testSpectrumFold
    testIqAssignment testSocket testSdkSampleClock testPairedCpiQueue
    testPipelineShutdown testPairedCallbacks)
  set_target_properties(${unit_test} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
endforeach()
