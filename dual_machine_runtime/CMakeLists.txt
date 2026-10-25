# Android NDK supplies CMake 3.22. The runtime only uses CMake language
# features available there, so keeping this floor lets the Windows host build
# reproducibly on the same workstation without a second CMake installation.
cmake_minimum_required(VERSION 3.22)

if(WIN32)
    # The repository root is intentionally descriptive and therefore deep.
    # Hash long object-path prefixes so Ninja can create every generated
    # directory on Windows without changing the source layout.
    set(CMAKE_OBJECT_PATH_MAX 128 CACHE STRING
        "Maximum object path length for Windows native builds")
endif()

set(VFDUAL_RELEASE_VERSION_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/release_version.txt")
if(NOT EXISTS "${VFDUAL_RELEASE_VERSION_FILE}")
    message(FATAL_ERROR
        "Dual-machine release version is missing: ${VFDUAL_RELEASE_VERSION_FILE}")
endif()
file(READ "${VFDUAL_RELEASE_VERSION_FILE}" VFDUAL_RELEASE_VERSION)
string(STRIP "${VFDUAL_RELEASE_VERSION}" VFDUAL_RELEASE_VERSION)
if(NOT VFDUAL_RELEASE_VERSION MATCHES
        "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "Dual-machine release version must be stable SemVer: ${VFDUAL_RELEASE_VERSION}")
endif()
set(VFDUAL_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(VFDUAL_VERSION_MINOR "${CMAKE_MATCH_2}")
set(VFDUAL_VERSION_PATCH "${CMAKE_MATCH_3}")
set(VFDUAL_WINDOWS_FILE_VERSION "${VFDUAL_RELEASE_VERSION}.0")

project(VisionForgeDualMachine VERSION "${VFDUAL_RELEASE_VERSION}" LANGUAGES CXX)

# Build entry points verify the real /c /showIncludes channel and project
# header dependency edges; /EP can emit a different encoding on Windows.

option(VFDUAL_ENABLE_PRIVATE_HOST_SYMBOLS
    "Generate and privately archive VisionForgeHost symbols without shipping them" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ninja and Makefile generators are single-config. Reusing one build tree for
# Debug and Release can leave MSVC STL/CRT objects with incompatible ABI flags
# even after CMake regenerates, producing misleading test crashes. Require a
# fresh build directory instead of silently accepting a configuration switch.
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    if(DEFINED VFDUAL_SINGLE_CONFIG_BUILD_TYPE AND
            NOT VFDUAL_SINGLE_CONFIG_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
        message(FATAL_ERROR
            "Single-config build directory was initialized for "
            "${VFDUAL_SINGLE_CONFIG_BUILD_TYPE}, not ${CMAKE_BUILD_TYPE}; "
            "use a fresh build directory")
    endif()
    set(VFDUAL_SINGLE_CONFIG_BUILD_TYPE "${CMAKE_BUILD_TYPE}"
        CACHE INTERNAL "Immutable build type for a single-config tree")
endif()

if(MSVC)
    # The Host is distributed as one EXE. Keep the MSVC/UCRT runtime inside
    # every Windows binary produced by this standalone runtime project.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/pdb")
    # Build the host runtime as UTF-8 regardless of the Windows system code
    # page. Chinese Windows defaults to CP936, which can misparse UTF-8 source
    # comments/strings and make release builds non-reproducible across systems.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

add_library(vfdual_protocol STATIC
    shared/src/access_unit_fragmenter.cpp
    shared/src/access_unit_reassembler.cpp
    shared/src/h264_annex_b_access_units.cpp
    shared/src/image_preprocessor.cpp
    shared/src/mouse_button_protocol.cpp
    shared/src/protocol.cpp
)
target_include_directories(vfdual_protocol PUBLIC shared/include)
target_compile_definitions(vfdual_protocol PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_executable(VisionForgePreprocessI420 tools/preprocess_yuv420_main.cpp)
target_link_libraries(VisionForgePreprocessI420 PRIVATE vfdual_protocol)
if(WIN32)
    target_link_libraries(vfdual_protocol PRIVATE bcrypt)
endif()

if(WIN32)
    set(VFDUAL_GENERATED_RESOURCE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/generated/host")
    file(MAKE_DIRECTORY "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}")
    file(MAKE_DIRECTORY "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}/vfdual")
    configure_file(
        host/windows/include/vfdual/host_release_version.hpp.in
        "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}/vfdual/host_release_version.hpp"
        @ONLY)
    set(VFDUAL_HOST_MANIFEST_PATH
        "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}/VisionForgeHost.manifest")
    configure_file(
        host/windows/VisionForgeHost.manifest
        "${VFDUAL_HOST_MANIFEST_PATH}"
        @ONLY)
    set(VFDUAL_HOST_ASSET_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/host/windows/assets/ui")
    set(VFDUAL_HOST_RESOURCE_FILE
        "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}/streamer_ui.rc")
    configure_file(
        host/windows/streamer_ui.rc
        "${VFDUAL_HOST_RESOURCE_FILE}"
        @ONLY)

    add_executable(VisionForgeHost WIN32
        "${VFDUAL_HOST_RESOURCE_FILE}"
        host/windows/src/streamer_desktop_main.cpp
        host/windows/src/host_crash_diagnostics.cpp
        host/windows/src/streamer_desktop_app.cpp
        host/windows/src/host_window_layout.cpp
        host/windows/src/host_display_catalog.cpp
        host/windows/src/host_cat6_protocol.cpp
        host/windows/src/host_cat6_session.cpp
        host/windows/src/host_cat6_bootstrap.cpp
        host/windows/src/host_cat6_bootstrap_wait.cpp
        host/windows/src/host_direct_link_provisioner.cpp
        host/windows/src/host_firewall_provisioner.cpp
        host/windows/src/isolated_dhcp_protocol.cpp
        host/windows/src/isolated_dhcp_server.cpp
        host/windows/src/host_cat6_monitor.cpp
        host/windows/src/host_runtime_service.cpp
        host/windows/src/host_ui_telemetry.cpp
        host/windows/src/host_outbound_rate_tracker.cpp
        host/windows/src/host_application.cpp
        host/windows/src/host_mouse_button_publisher.cpp
        host/windows/src/host_runtime_config.cpp
        host/windows/src/idr_request_listener.cpp
        host/windows/src/streamer_metrics_csv.cpp
        host/windows/src/desktop_video_agent.cpp
        host/windows/src/hybrid_gpu_video_pipeline.cpp
        host/windows/src/d3d11_frame_bridge.cpp
        host/windows/src/dxgi_desktop_capture.cpp
        host/windows/src/media_foundation_h264_encoder.cpp
        host/windows/src/nvenc_h264_encoder.cpp
        host/windows/src/nvenc_runtime_probe.cpp
        host/windows/src/nvenc_session_probe.cpp
        host/windows/src/udp_socket.cpp
        host/windows/src/udp_video_publisher.cpp
        host/windows/src/encoded_clip_recorder.cpp
    )
    set_target_properties(VisionForgeHost PROPERTIES OUTPUT_NAME VFHost)
    if(MSVC)
        # Keep the latency-sensitive Host native and fully optimized while
        # making the Windows exploit mitigations explicit.  These switches do
        # not schedule, sleep or cap the capture/publish loop.
        target_compile_options(VisionForgeHost PRIVATE
            "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/GS>"
            "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/sdl>"
            "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/guard:cf>")
        target_link_options(VisionForgeHost PRIVATE
            "$<$<CONFIG:Release>:/DYNAMICBASE>"
            "$<$<CONFIG:Release>:/HIGHENTROPYVA>"
            "$<$<CONFIG:Release>:/NXCOMPAT>"
            "$<$<CONFIG:Release>:/guard:cf>"
            "$<$<CONFIG:Release>:/CETCOMPAT>")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT vfdual_host_ipo_supported
                            OUTPUT vfdual_host_ipo_error)
        if(vfdual_host_ipo_supported)
            set_property(TARGET VisionForgeHost PROPERTY
                INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(FATAL_ERROR
                "Release Host requires MSVC IPO/LTCG: ${vfdual_host_ipo_error}")
        endif()
    endif()
    target_include_directories(VisionForgeHost PRIVATE
        "${VFDUAL_GENERATED_RESOURCE_DIRECTORY}"
        host/windows
        host/windows/include
        third_party/nv-codec-headers/include)
    target_link_libraries(VisionForgeHost PRIVATE vfdual_protocol ws2_32 iphlpapi uuid advapi32 bcrypt crypt32 ncrypt tbs d3d11 dxgi
        mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 user32 gdi32 shell32 dwmapi dbghelp)
    if(MSVC AND VFDUAL_ENABLE_PRIVATE_HOST_SYMBOLS)
        target_compile_options(VisionForgeHost PRIVATE /Zi /FS)
        target_link_options(VisionForgeHost PRIVATE
            /DEBUG:FULL /OPT:REF /OPT:ICF /PDBALTPATH:VisionForgeHost.pdb)
        find_program(VFDUAL_POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
        add_custom_command(TARGET VisionForgeHost POST_BUILD
            COMMAND "${VFDUAL_POWERSHELL_EXECUTABLE}"
                -NoProfile
                -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/host/windows/archive_host_symbols.ps1"
                -ExePath "$<TARGET_FILE:VisionForgeHost>"
                -PdbPath "$<TARGET_PDB_FILE:VisionForgeHost>"
                -ArchiveDirectory "${CMAKE_CURRENT_BINARY_DIR}/private_symbols"
            COMMENT "Archiving private VisionForgeHost symbols by executable SHA-256"
            VERBATIM)
    endif()

    # Deterministic, repository-owned 144 Hz visual source used by physical
    # capture acceptance. It is not shipped with the production Host binary.
    add_executable(VisionForgeCaptureStimulus WIN32
        host/windows/tests/capture_stimulus_main.cpp
    )
    target_link_libraries(VisionForgeCaptureStimulus PRIVATE vfdual_protocol d3d11 dxgi user32 shell32)

    add_executable(VisionForgeDatasetReplay
        host/windows/src/dataset_h264_replayer.cpp
        host/windows/src/udp_socket.cpp
        host/windows/src/udp_video_publisher.cpp
        host/windows/src/encoded_clip_recorder.cpp
    )
    target_include_directories(VisionForgeDatasetReplay PRIVATE host/windows/include)
    target_link_libraries(VisionForgeDatasetReplay PRIVATE vfdual_protocol ws2_32)

    add_executable(VisionForgeDatasetEvaluate
        host/windows/src/dataset_detection_evaluator.cpp
    )
    target_link_libraries(VisionForgeDatasetEvaluate PRIVATE vfdual_protocol)
    add_test(NAME vfdual_dataset_evaluator_tests COMMAND VisionForgeDatasetEvaluate
        --labels tests/data/dataset_evaluation/labels
        --trace tests/data/dataset_evaluation/trace.csv
        --max-frames 2
        --require-complete
    )
    set_tests_properties(vfdual_dataset_evaluator_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(VisionForgeControlTraceEvaluate
        host/windows/src/mobile_control_trace_evaluator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp
    )
    target_include_directories(VisionForgeControlTraceEvaluate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    set(vfdual_control_trace_contract_script
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_control_trace_evaluator.cmake)
    add_test(NAME vfdual_control_trace_evaluator_tests
        COMMAND ${CMAKE_COMMAND}
            "-DVFDUAL_CONTROL_TRACE_EVALUATOR=$<TARGET_FILE:VisionForgeControlTraceEvaluate>"
            "-DVFDUAL_CONTROL_TRACE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -DVFDUAL_CONTROL_TRACE_CASE=normal
            -P "${vfdual_control_trace_contract_script}")

    add_test(NAME vfdual_control_trace_evaluator_stale_guard_tests
        COMMAND ${CMAKE_COMMAND}
            "-DVFDUAL_CONTROL_TRACE_EVALUATOR=$<TARGET_FILE:VisionForgeControlTraceEvaluate>"
            "-DVFDUAL_CONTROL_TRACE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -DVFDUAL_CONTROL_TRACE_CASE=stale
            -P "${vfdual_control_trace_contract_script}")

    add_test(NAME vfdual_control_trace_evaluator_incomplete_guard_tests
        COMMAND ${CMAKE_COMMAND}
            "-DVFDUAL_CONTROL_TRACE_EVALUATOR=$<TARGET_FILE:VisionForgeControlTraceEvaluate>"
            "-DVFDUAL_CONTROL_TRACE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -DVFDUAL_CONTROL_TRACE_CASE=incomplete
            -P "${vfdual_control_trace_contract_script}")

    add_test(NAME vfdual_control_trace_evaluator_reacquisition_tests
        COMMAND ${CMAKE_COMMAND}
            "-DVFDUAL_CONTROL_TRACE_EVALUATOR=$<TARGET_FILE:VisionForgeControlTraceEvaluate>"
            "-DVFDUAL_CONTROL_TRACE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -DVFDUAL_CONTROL_TRACE_CASE=reacquisition
            -P "${vfdual_control_trace_contract_script}")

    add_test(NAME vfdual_control_trace_evaluator_high_fps_reacquisition_tests
        COMMAND ${CMAKE_COMMAND}
            "-DVFDUAL_CONTROL_TRACE_EVALUATOR=$<TARGET_FILE:VisionForgeControlTraceEvaluate>"
            "-DVFDUAL_CONTROL_TRACE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -DVFDUAL_CONTROL_TRACE_CASE=high_fps_reacquisition
            -P "${vfdual_control_trace_contract_script}")

    add_executable(vfdual_mobile_background_motion_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileBackgroundMotionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileBackgroundMotion.cpp)
    target_include_directories(vfdual_mobile_background_motion_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_background_motion_tests COMMAND vfdual_mobile_background_motion_tests)

    add_executable(vfdual_mobile_target_image_motion_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetImageMotionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetImageMotion.cpp)
    target_include_directories(vfdual_mobile_target_image_motion_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_target_image_motion_tests COMMAND vfdual_mobile_target_image_motion_tests)

    add_executable(vfdual_qnn_execution_profile_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/QnnExecutionProfileTests.cpp)
    target_include_directories(vfdual_qnn_execution_profile_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/qnn/include/QNN)
    add_test(NAME vfdual_qnn_execution_profile_tests COMMAND vfdual_qnn_execution_profile_tests)

    add_executable(vfdual_pipeline_timing_trace_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/PipelineTimingTraceTests.cpp)
    add_test(NAME vfdual_pipeline_timing_trace_tests COMMAND vfdual_pipeline_timing_trace_tests)

    add_executable(vfdual_decoded_observation_admission_tests
        ../android_inference_benchmark/app/src/main/cpp/DecodedObservationAdmissionTests.cpp)
    add_test(NAME vfdual_decoded_observation_admission_tests
        COMMAND vfdual_decoded_observation_admission_tests)

    add_executable(vfdual_control_observation_domain_tests
        ../android_inference_benchmark/app/src/main/cpp/ControlObservationDomainTests.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileBackgroundMotion.cpp)
    target_include_directories(vfdual_control_observation_domain_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp shared/include)
    add_test(NAME vfdual_control_observation_domain_tests
        COMMAND vfdual_control_observation_domain_tests)

    add_executable(vfdual_mobile_pixel_patch_search_reuse_tests
        ../android_inference_benchmark/app/src/main/cpp/MobilePixelPatchSearchReuseTests.cpp)
    add_test(NAME vfdual_mobile_pixel_patch_search_reuse_tests
        COMMAND vfdual_mobile_pixel_patch_search_reuse_tests)

    add_executable(vfdual_mobile_image_history_tests
        ../android_inference_benchmark/app/src/main/cpp/MobileImageHistoryTests.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileBackgroundMotion.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileTargetImageMotion.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_image_history_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp shared/include)
    add_test(NAME vfdual_mobile_image_history_tests
        COMMAND vfdual_mobile_image_history_tests)

    add_executable(vfdual_mobile_patch_sad_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobilePatchSadTests.cpp)
    add_test(NAME vfdual_mobile_patch_sad_tests COMMAND vfdual_mobile_patch_sad_tests)

    add_executable(vfdual_mobile_rgb_area_prefilter_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileRgbAreaPrefilterTests.cpp)
    add_test(NAME vfdual_mobile_rgb_area_prefilter_tests COMMAND vfdual_mobile_rgb_area_prefilter_tests)

    add_executable(vfdual_mobile_pixel_patch_tests
        ../android_inference_benchmark/app/src/main/cpp/MobilePixelPatchTests.cpp)
    target_include_directories(vfdual_mobile_pixel_patch_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_pixel_patch_tests COMMAND vfdual_mobile_pixel_patch_tests)

    add_executable(vfdual_mobile_fresh_pixel_projection_tests
        ../android_inference_benchmark/app/src/main/cpp/MobileFreshPixelProjectionTests.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileFreshPixelProjection.cpp)
    target_include_directories(vfdual_mobile_fresh_pixel_projection_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_fresh_pixel_projection_tests COMMAND vfdual_mobile_fresh_pixel_projection_tests)

    add_executable(vfdual_projection_observation_gate_tests
        ../android_inference_benchmark/app/src/main/cpp/ProjectionObservationGateTests.cpp)
    add_test(NAME vfdual_projection_observation_gate_tests COMMAND vfdual_projection_observation_gate_tests)

    add_executable(vfdual_projection_timing_trace_tests
        ../android_inference_benchmark/app/src/main/cpp/ProjectionTimingTraceTests.cpp)
    add_test(NAME vfdual_projection_timing_trace_tests COMMAND vfdual_projection_timing_trace_tests)

    add_executable(vfdual_disturbance_selection_trace_tests
        ../android_inference_benchmark/app/src/main/cpp/DisturbanceSelectionTraceTests.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_disturbance_selection_trace_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp
        shared/include)
    add_test(NAME vfdual_disturbance_selection_trace_tests COMMAND vfdual_disturbance_selection_trace_tests)

    add_executable(vfdual_native_decoder_report_snapshot_tests
        ../android_inference_benchmark/app/src/main/cpp/NativeDecoderReportSnapshotTests.cpp)
    target_include_directories(vfdual_native_decoder_report_snapshot_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp
        shared/include)
    add_test(NAME vfdual_native_decoder_report_snapshot_tests COMMAND vfdual_native_decoder_report_snapshot_tests)

    add_executable(vfdual_mobile_image_identity_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileImageIdentityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_image_identity_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_image_identity_tests COMMAND vfdual_mobile_image_identity_tests)

    add_executable(vfdual_mobile_image_association_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileImageAssociationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetImageMotion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_image_association_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_image_association_tests COMMAND vfdual_mobile_image_association_tests)

    add_executable(vfdual_mobile_target_tracker_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp
    )
    target_include_directories(vfdual_mobile_target_tracker_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_target_tracker_tests COMMAND vfdual_mobile_target_tracker_tests)

    add_executable(VisionForgeCrowdCorrespondenceAudit
        tools/crowd_correspondence_audit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(VisionForgeCrowdCorrespondenceAudit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)

    add_executable(vfdual_mobile_large_motion_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileLargeMotionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileBackgroundMotion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_large_motion_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_large_motion_tests COMMAND vfdual_mobile_large_motion_tests)

    add_executable(vfdual_mobile_acquisition_latency_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileAcquisitionLatencyTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_acquisition_latency_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_acquisition_latency_tests COMMAND vfdual_mobile_acquisition_latency_tests)

    add_executable(vfdual_mobile_observation_servo_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileObservationServoTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_observation_servo_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_observation_servo_tests COMMAND vfdual_mobile_observation_servo_tests)

    add_executable(vfdual_mobile_control_audit_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlAuditTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_control_audit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_control_audit_tests COMMAND vfdual_mobile_control_audit_tests)

    add_executable(VisionForgeGameMotionDiagnostics
        tools/game_motion_diagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(VisionForgeGameMotionDiagnostics PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_game_projection_geometry_tests COMMAND VisionForgeGameMotionDiagnostics --geometry-only)

    add_executable(vfdual_mobile_control_core_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCoreTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp
    )
    target_include_directories(vfdual_mobile_control_core_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_control_core_tests COMMAND vfdual_mobile_control_core_tests)

    add_executable(vfdual_inference_deadline_admission_tests
        ../android_inference_benchmark/app/src/main/cpp/InferenceDeadlineAdmissionTests.cpp)
    target_include_directories(vfdual_inference_deadline_admission_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_inference_deadline_admission_tests COMMAND vfdual_inference_deadline_admission_tests)
    add_executable(vfdual_atomic_duration_summary_tests
        ../android_inference_benchmark/app/src/main/cpp/AtomicDurationSummaryTests.cpp)
    target_include_directories(vfdual_atomic_duration_summary_tests PRIVATE
        ../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_atomic_duration_summary_tests COMMAND vfdual_atomic_duration_summary_tests)

    # The bridge's ticket/session interaction needs the production translation
    # unit, not only independent gate tests. JDK headers provide the JNI types;
    # this desktop regression never creates a VM or opens an output device.
    find_path(VFDUAL_TEST_JNI_INCLUDE jni.h HINTS "$ENV{JAVA_HOME}/include")
    if(WIN32 AND VFDUAL_TEST_JNI_INCLUDE)
        add_executable(vfdual_makcu_bridge_deadline_tests
            ../android_inference_benchmark/app/src/main/cpp/MakcuBridgeDeadlineTests.cpp
            ../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
            ../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
            ../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
        target_include_directories(vfdual_makcu_bridge_deadline_tests PRIVATE
            ../android_inference_benchmark/app/src/main/cpp shared/include
            "${VFDUAL_TEST_JNI_INCLUDE}" "${VFDUAL_TEST_JNI_INCLUDE}/win32")
        add_test(NAME vfdual_makcu_bridge_deadline_tests COMMAND vfdual_makcu_bridge_deadline_tests)
    endif()

    add_executable(vfdual_mobile_motion_planner_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlannerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp)
    target_include_directories(vfdual_mobile_motion_planner_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_mobile_motion_planner_tests COMMAND vfdual_mobile_motion_planner_tests)

    add_executable(vfdual_mobile_control_allocation_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlAllocationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileControlCore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileMotionPlanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MobileTargetTracker.cpp)
    target_include_directories(vfdual_mobile_control_allocation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_mobile_control_allocation_tests COMMAND vfdual_mobile_control_allocation_tests)

    add_executable(vfdual_control_blocker_state_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ControlBlockerStateTests.cpp)
    target_include_directories(vfdual_control_blocker_state_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_control_blocker_state_tests COMMAND vfdual_control_blocker_state_tests)

    add_executable(vfdual_receiver_recovery_policy_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverRecoveryPolicyTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverRecoveryPolicy.cpp
    )
    target_include_directories(vfdual_receiver_recovery_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_receiver_recovery_policy_tests COMMAND vfdual_receiver_recovery_policy_tests)

    add_executable(vfdual_receiver_repeat_resync_policy_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverRepeatResyncPolicyTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverRecoveryPolicy.cpp
    )
    target_include_directories(vfdual_receiver_repeat_resync_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_receiver_repeat_resync_policy_tests
        COMMAND vfdual_receiver_repeat_resync_policy_tests)

    add_executable(vfdual_receiver_reference_sync_policy_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverReferenceSyncPolicyTests.cpp
    )
    target_include_directories(vfdual_receiver_reference_sync_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_receiver_reference_sync_policy_tests
        COMMAND vfdual_receiver_reference_sync_policy_tests)

    add_executable(vfdual_receiver_idle_backoff_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverIdleBackoffTests.cpp
    )
    target_include_directories(vfdual_receiver_idle_backoff_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_receiver_idle_backoff_tests COMMAND vfdual_receiver_idle_backoff_tests)

    add_executable(vfdual_receiver_source_session_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverSourceSessionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/ReceiverSourceSession.cpp
    )
    target_include_directories(vfdual_receiver_source_session_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    target_link_libraries(vfdual_receiver_source_session_tests PRIVATE vfdual_protocol)
    add_test(NAME vfdual_receiver_source_session_tests COMMAND vfdual_receiver_source_session_tests)

    add_executable(vfdual_makcu_output_gate_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/MakcuOutputGateTests.cpp
    )
    target_include_directories(vfdual_makcu_output_gate_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_makcu_output_gate_tests COMMAND vfdual_makcu_output_gate_tests)

    add_executable(vfdual_yolo_postprocessor_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/YoloPostprocessorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/YoloPostprocessor.cpp
    )
    target_include_directories(vfdual_yolo_postprocessor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/shared/include)
    add_test(NAME vfdual_yolo_postprocessor_tests COMMAND vfdual_yolo_postprocessor_tests)

    add_executable(vfdual_decoder_input_drop_metrics_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp/DecoderInputDropMetricsTests.cpp
    )
    target_include_directories(vfdual_decoder_input_drop_metrics_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../android_inference_benchmark/app/src/main/cpp)
    add_test(NAME vfdual_decoder_input_drop_metrics_tests
        COMMAND vfdual_decoder_input_drop_metrics_tests)

endif()

enable_testing()
add_executable(vfdual_protocol_tests tests/protocol_tests.cpp)
target_link_libraries(vfdual_protocol_tests PRIVATE vfdual_protocol)
add_test(NAME vfdual_protocol_tests COMMAND vfdual_protocol_tests)

# The implementation is included once so this test exercises both private
# scalar and NEON kernels. Host skips are explicit; ARM checks run on-device.
add_executable(vfdual_native_nv12_parity_tests tests/native_nv12_parity_tests.cpp)
target_include_directories(vfdual_native_nv12_parity_tests PRIVATE shared/include)
add_test(NAME vfdual_native_nv12_parity_tests COMMAND vfdual_native_nv12_parity_tests)
set_tests_properties(vfdual_native_nv12_parity_tests PROPERTIES SKIP_RETURN_CODE 77)

add_executable(vfdual_reassembly_recovery_tests tests/reassembly_recovery_tests.cpp)
target_link_libraries(vfdual_reassembly_recovery_tests PRIVATE vfdual_protocol)
add_test(NAME vfdual_reassembly_recovery_tests COMMAND vfdual_reassembly_recovery_tests)

if(WIN32)
    add_executable(vfdual_dxgi_capture_tests
        host/windows/tests/dxgi_desktop_capture_tests.cpp
        host/windows/src/dxgi_desktop_capture.cpp
        host/windows/src/host_display_catalog.cpp
    )
    target_include_directories(vfdual_dxgi_capture_tests PRIVATE
        host/windows/include shared/include)
    target_link_libraries(vfdual_dxgi_capture_tests PRIVATE d3d11 dxgi user32 dwmapi)
    add_test(NAME vfdual_dxgi_capture_tests COMMAND vfdual_dxgi_capture_tests)
    set_tests_properties(vfdual_dxgi_capture_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(vfdual_dxgi_frame_semantics_tests
        host/windows/tests/dxgi_frame_semantics_tests.cpp
    )
    target_include_directories(vfdual_dxgi_frame_semantics_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_dxgi_frame_semantics_tests COMMAND vfdual_dxgi_frame_semantics_tests)

    add_executable(vfdual_host_encoder_policy_tests
        host/windows/tests/host_encoder_policy_tests.cpp
    )
    target_include_directories(vfdual_host_encoder_policy_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_encoder_policy_tests
        COMMAND vfdual_host_encoder_policy_tests)

    add_executable(vfdual_bgra_nv12_tests host/windows/tests/bgra_nv12_tests.cpp)
    target_include_directories(vfdual_bgra_nv12_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_bgra_nv12_tests COMMAND vfdual_bgra_nv12_tests)

    add_executable(vfdual_hybrid_gpu_pipeline_policy_tests
        host/windows/tests/hybrid_gpu_video_pipeline_policy_tests.cpp
    )
    target_include_directories(vfdual_hybrid_gpu_pipeline_policy_tests PRIVATE
        host/windows/include shared/include)
    target_compile_definitions(vfdual_hybrid_gpu_pipeline_policy_tests
        PRIVATE VFDUAL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME vfdual_hybrid_gpu_pipeline_policy_tests
        COMMAND vfdual_hybrid_gpu_pipeline_policy_tests)

    add_executable(vfdual_nvenc_runtime_probe_tests
        host/windows/tests/nvenc_runtime_probe_tests.cpp
        host/windows/src/nvenc_runtime_probe.cpp
    )
    target_include_directories(vfdual_nvenc_runtime_probe_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_nvenc_runtime_probe_tests COMMAND vfdual_nvenc_runtime_probe_tests)
    set_tests_properties(vfdual_nvenc_runtime_probe_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(vfdual_nvenc_session_probe_tests
        host/windows/tests/nvenc_session_probe_tests.cpp
        host/windows/src/nvenc_session_probe.cpp
        host/windows/src/nvenc_h264_encoder.cpp
    )
    target_include_directories(vfdual_nvenc_session_probe_tests PRIVATE host/windows/include third_party/nv-codec-headers/include)
    target_link_libraries(vfdual_nvenc_session_probe_tests PRIVATE d3d11 dxgi)
    add_test(NAME vfdual_nvenc_session_probe_tests COMMAND vfdual_nvenc_session_probe_tests)
    set_tests_properties(vfdual_nvenc_session_probe_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(vfdual_desktop_video_agent_tests
        host/windows/tests/desktop_video_agent_tests.cpp
        host/windows/src/desktop_video_agent.cpp
        host/windows/src/hybrid_gpu_video_pipeline.cpp
        host/windows/src/d3d11_frame_bridge.cpp
        host/windows/src/dxgi_desktop_capture.cpp
        host/windows/src/host_display_catalog.cpp
        host/windows/src/media_foundation_h264_encoder.cpp
        host/windows/src/nvenc_h264_encoder.cpp
        host/windows/src/udp_socket.cpp
        host/windows/src/udp_video_publisher.cpp
        host/windows/src/encoded_clip_recorder.cpp
    )
    target_include_directories(vfdual_desktop_video_agent_tests PRIVATE
        host/windows/include shared/include third_party/nv-codec-headers/include)
    target_link_libraries(vfdual_desktop_video_agent_tests PRIVATE vfdual_protocol ws2_32 d3d11 dxgi
        mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 user32 dwmapi)
    add_test(NAME vfdual_desktop_video_agent_tests COMMAND vfdual_desktop_video_agent_tests)
    set_tests_properties(vfdual_desktop_video_agent_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(VisionForgeHostProfile
        tools/host_pipeline_profile_main.cpp
        host/windows/src/desktop_video_agent.cpp
        host/windows/src/hybrid_gpu_video_pipeline.cpp
        host/windows/src/d3d11_frame_bridge.cpp
        host/windows/src/dxgi_desktop_capture.cpp
        host/windows/src/host_display_catalog.cpp
        host/windows/src/media_foundation_h264_encoder.cpp
        host/windows/src/nvenc_h264_encoder.cpp
        host/windows/src/udp_socket.cpp
        host/windows/src/udp_video_publisher.cpp
        host/windows/src/encoded_clip_recorder.cpp
        host/windows/src/streamer_metrics_csv.cpp)
    target_include_directories(VisionForgeHostProfile PRIVATE
        host/windows/include shared/include third_party/nv-codec-headers/include)
    target_link_libraries(VisionForgeHostProfile PRIVATE vfdual_protocol ws2_32 d3d11 dxgi
        mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32 user32 dwmapi)

    add_executable(VisionForgeGpuPhaseProfile
        tools/gpu_phase_profile_main.cpp
        host/windows/src/d3d11_frame_bridge.cpp
        host/windows/src/dxgi_desktop_capture.cpp
        host/windows/src/host_display_catalog.cpp
        host/windows/src/nvenc_h264_encoder.cpp)
    target_include_directories(VisionForgeGpuPhaseProfile PRIVATE
        host/windows/include shared/include third_party/nv-codec-headers/include)
    target_link_libraries(VisionForgeGpuPhaseProfile PRIVATE d3d11 dxgi user32 dwmapi)

    add_executable(vfdual_media_foundation_encoder_tests
        host/windows/tests/media_foundation_h264_encoder_tests.cpp
        host/windows/src/media_foundation_h264_encoder.cpp
    )
    target_include_directories(vfdual_media_foundation_encoder_tests PRIVATE host/windows/include)
    target_link_libraries(vfdual_media_foundation_encoder_tests PRIVATE d3d11 dxgi
        mfplat mf mfuuid wmcodecdspuuid ole32 oleaut32)
    add_test(NAME vfdual_media_foundation_encoder_tests COMMAND vfdual_media_foundation_encoder_tests)

    add_executable(vfdual_isolated_dhcp_protocol_tests
        host/windows/tests/isolated_dhcp_protocol_tests.cpp
        host/windows/src/isolated_dhcp_protocol.cpp
        host/windows/src/isolated_dhcp_server.cpp
    )
    target_include_directories(vfdual_isolated_dhcp_protocol_tests PRIVATE host/windows/include)
    target_compile_definitions(vfdual_isolated_dhcp_protocol_tests
        PRIVATE VFDUAL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(vfdual_isolated_dhcp_protocol_tests PRIVATE ws2_32 iphlpapi)
    add_test(NAME vfdual_isolated_dhcp_protocol_tests COMMAND vfdual_isolated_dhcp_protocol_tests)

    add_executable(vfdual_host_cat6_protocol_tests
        host/windows/tests/host_cat6_protocol_tests.cpp
        host/windows/src/host_cat6_protocol.cpp
    )
    target_include_directories(vfdual_host_cat6_protocol_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_cat6_protocol_tests COMMAND vfdual_host_cat6_protocol_tests)

    add_executable(vfdual_host_cat6_bootstrap_wait_tests
        host/windows/tests/host_cat6_bootstrap_wait_tests.cpp
        host/windows/src/host_cat6_bootstrap_wait.cpp
    )
    target_include_directories(vfdual_host_cat6_bootstrap_wait_tests PRIVATE host/windows/include)
    target_compile_definitions(vfdual_host_cat6_bootstrap_wait_tests
        PRIVATE VFDUAL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME vfdual_host_cat6_bootstrap_wait_tests COMMAND vfdual_host_cat6_bootstrap_wait_tests)

    add_executable(vfdual_host_cat6_monitor_policy_tests
        host/windows/tests/host_cat6_monitor_policy_tests.cpp
        host/windows/src/host_cat6_monitor.cpp
        host/windows/src/host_cat6_session.cpp
        host/windows/src/host_cat6_protocol.cpp
    )
    target_include_directories(vfdual_host_cat6_monitor_policy_tests PRIVATE host/windows/include)
    target_compile_definitions(vfdual_host_cat6_monitor_policy_tests
        PRIVATE VFDUAL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(vfdual_host_cat6_monitor_policy_tests PRIVATE ws2_32 iphlpapi)
    add_test(NAME vfdual_host_cat6_monitor_policy_tests
        COMMAND vfdual_host_cat6_monitor_policy_tests)

    add_executable(vfdual_host_direct_link_policy_tests
        host/windows/tests/host_direct_link_policy_tests.cpp
        host/windows/src/host_direct_link_provisioner.cpp
        host/windows/src/host_firewall_provisioner.cpp
    )
    target_include_directories(vfdual_host_direct_link_policy_tests PRIVATE host/windows/include)
    target_link_libraries(vfdual_host_direct_link_policy_tests PRIVATE ws2_32 iphlpapi uuid advapi32 ole32 oleaut32)
    add_test(NAME vfdual_host_direct_link_policy_tests COMMAND vfdual_host_direct_link_policy_tests)

    add_executable(vfdual_host_firewall_policy_tests
        host/windows/tests/host_firewall_policy_tests.cpp
        host/windows/src/host_firewall_provisioner.cpp
    )
    target_include_directories(vfdual_host_firewall_policy_tests PRIVATE host/windows/include)
    target_link_libraries(vfdual_host_firewall_policy_tests PRIVATE uuid advapi32 ole32 oleaut32)
    add_test(NAME vfdual_host_firewall_policy_tests COMMAND vfdual_host_firewall_policy_tests)

    add_executable(vfdual_host_display_catalog_tests
        host/windows/tests/host_display_catalog_tests.cpp
        host/windows/src/host_display_catalog.cpp
    )
    target_include_directories(vfdual_host_display_catalog_tests PRIVATE
        host/windows/include shared/include)
    target_link_libraries(vfdual_host_display_catalog_tests PRIVATE dxgi user32 dwmapi)
    add_test(NAME vfdual_host_display_catalog_tests COMMAND vfdual_host_display_catalog_tests)

    add_executable(vfdual_host_outbound_rate_tracker_tests
        host/windows/tests/host_outbound_rate_tracker_tests.cpp
        host/windows/src/host_outbound_rate_tracker.cpp
    )
    target_include_directories(vfdual_host_outbound_rate_tracker_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_outbound_rate_tracker_tests COMMAND vfdual_host_outbound_rate_tracker_tests)

    add_executable(vfdual_host_ui_telemetry_tests
        host/windows/tests/host_ui_telemetry_tests.cpp
        host/windows/src/host_ui_telemetry.cpp
        host/windows/src/host_outbound_rate_tracker.cpp
    )
    target_include_directories(vfdual_host_ui_telemetry_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_ui_telemetry_tests COMMAND vfdual_host_ui_telemetry_tests)

    add_executable(vfdual_host_window_layout_tests
        host/windows/tests/host_window_layout_tests.cpp
        host/windows/src/host_window_layout.cpp
    )
    target_include_directories(vfdual_host_window_layout_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_window_layout_tests COMMAND vfdual_host_window_layout_tests)

    add_executable(vfdual_host_recovery_policy_tests
        host/windows/tests/host_recovery_policy_tests.cpp
    )
    target_include_directories(vfdual_host_recovery_policy_tests PRIVATE host/windows/include)
    add_test(NAME vfdual_host_recovery_policy_tests COMMAND vfdual_host_recovery_policy_tests)

    add_executable(vfdual_host_published_frame_accounting_tests
        host/windows/tests/host_published_frame_accounting_tests.cpp
    )
    target_include_directories(
        vfdual_host_published_frame_accounting_tests PRIVATE
        host/windows/include
        shared/include)
    add_test(NAME vfdual_host_published_frame_accounting_tests
        COMMAND vfdual_host_published_frame_accounting_tests)

    add_executable(vfdual_streamer_metrics_csv_tests
        host/windows/tests/streamer_metrics_csv_tests.cpp
        host/windows/src/streamer_metrics_csv.cpp
    )
    target_include_directories(vfdual_streamer_metrics_csv_tests PRIVATE
        host/windows/include shared/include)
    target_link_libraries(vfdual_streamer_metrics_csv_tests PRIVATE user32)
    add_test(NAME vfdual_streamer_metrics_csv_tests
        COMMAND vfdual_streamer_metrics_csv_tests)

    add_executable(vfdual_idr_request_listener_tests
        host/windows/tests/idr_request_listener_tests.cpp
        host/windows/src/idr_request_listener.cpp
        host/windows/src/udp_socket.cpp
    )
    target_include_directories(vfdual_idr_request_listener_tests PRIVATE host/windows/include)
    target_link_libraries(vfdual_idr_request_listener_tests PRIVATE ws2_32)
    add_test(NAME vfdual_idr_request_listener_tests COMMAND vfdual_idr_request_listener_tests)

    add_executable(vfdual_udp_video_publisher_contract_tests
        host/windows/tests/udp_video_publisher_contract_tests.cpp
        host/windows/src/udp_video_publisher.cpp
        host/windows/src/encoded_clip_recorder.cpp
        host/windows/src/udp_socket.cpp
    )
    target_include_directories(vfdual_udp_video_publisher_contract_tests PRIVATE host/windows/include)
    target_link_libraries(vfdual_udp_video_publisher_contract_tests PRIVATE vfdual_protocol ws2_32)
    add_test(NAME vfdual_udp_video_publisher_contract_tests
        COMMAND vfdual_udp_video_publisher_contract_tests)

    add_executable(vfdual_udp_socket_tests
        host/windows/tests/udp_socket_tests.cpp
        host/windows/src/udp_socket.cpp
    )
    target_include_directories(vfdual_udp_socket_tests
        PRIVATE host/windows/include)
    target_link_libraries(vfdual_udp_socket_tests PRIVATE ws2_32)
    add_test(NAME vfdual_udp_socket_tests COMMAND vfdual_udp_socket_tests)
endif()

if(WIN32)
    add_executable(VisionForgeRawMouseProbe tools/raw_mouse_probe_main.cpp)
    target_link_libraries(VisionForgeRawMouseProbe PRIVATE user32)
endif()

# The native contracts intentionally use the standard assert macro. Release
# configurations define NDEBUG globally, so undefine it for test executables
# only; otherwise CTest can report a false pass without evaluating a check.
get_property(vfdual_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
set(vfdual_test_build_targets)
foreach(vfdual_target IN LISTS vfdual_directory_targets)
    if(vfdual_target MATCHES "_tests$")
        list(APPEND vfdual_test_build_targets ${vfdual_target})
        if(MSVC)
            target_compile_options(${vfdual_target} PRIVATE /UNDEBUG)
            # Some contracts keep several complete trace-owning controller
            # fixtures on the stack. Android owns the live controller globally;
            # give Windows test fixtures room without changing runtime threads.
            target_link_options(${vfdual_target} PRIVATE /STACK:8388608)
        else()
            target_compile_options(${vfdual_target} PRIVATE -UNDEBUG)
        endif()
    endif()
endforeach()

# Build only CTest dependencies in the disposable test tree. The formal Host
# executable is already built in its own clean tree and must not be rebuilt or
# linked together with test objects from a different dependency database.
foreach(vfdual_evaluator_target
        VisionForgeDatasetEvaluate
        VisionForgeGameMotionDiagnostics
        VisionForgeControlTraceEvaluate)
    if(TARGET ${vfdual_evaluator_target})
        list(APPEND vfdual_test_build_targets ${vfdual_evaluator_target})
    endif()
endforeach()
add_custom_target(vfdual_test_build DEPENDS ${vfdual_test_build_targets})
