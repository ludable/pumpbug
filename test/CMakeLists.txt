# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

find_program(NODE_EXECUTABLE node REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(add_host_test name)
  add_executable(${name} ${name}.cpp ${ARGN})
  target_compile_features(${name} PRIVATE cxx_std_17)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_navigation_stack)
add_host_test(test_storage "${PROJECT_SOURCE_DIR}/src/util/storage.cpp")
target_include_directories(
  test_storage BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
add_host_test(test_battery_level)
add_host_test(
  test_low_battery_shutdown_policy
  "${PROJECT_SOURCE_DIR}/src/power/LowBatteryShutdownPolicy.cpp"
)
add_host_test(test_ble_startup_retry_policy)
add_host_test(test_pair_redirect_policy)
add_host_test(test_bluetooth_controller_transition)
add_host_test(test_wifi_power_save)
add_host_test(test_scale_radio_policy)
add_host_test(
  test_scale_radio_state_machine
  "${PROJECT_SOURCE_DIR}/src/power/ScaleRadioStateMachine.cpp"
)
add_host_test(
  test_scale_radio_convergence
  "${PROJECT_SOURCE_DIR}/src/power/ScaleRadioStateMachine.cpp"
)
add_host_test(
  test_scale_reading_timing_observer
  "${PROJECT_SOURCE_DIR}/src/apps/extraction/ScaleReadingTimingObserver.cpp"
)
target_compile_definitions(
  test_scale_reading_timing_observer
  PRIVATE PB_SCALE_READING_TIMING=1
)

add_host_test(
  test_speaker_codec
  "${PROJECT_SOURCE_DIR}/src/power/SpeakerCodec.cpp"
)
target_include_directories(
  test_speaker_codec BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

add_host_test(
  test_power_manager
  "${PROJECT_SOURCE_DIR}/src/power/LowBatteryShutdownPolicy.cpp"
  "${PROJECT_SOURCE_DIR}/src/power/PowerManager.cpp"
  "${PROJECT_SOURCE_DIR}/src/power/ScaleRadioStateMachine.cpp"
  "${PROJECT_SOURCE_DIR}/src/power/SpeakerCodec.cpp"
)
target_include_directories(
  test_power_manager BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

add_host_test(
  test_acaia_codec
  "${PROJECT_SOURCE_DIR}/src/ble/AcaiaV2Codec.cpp"
)
target_include_directories(
  test_acaia_codec PRIVATE "${PROJECT_SOURCE_DIR}/src/ble"
)

foreach(name
    test_flow_estimator
    test_live_pour_tracker
    test_pour_evidence
    test_extraction_diagnosis
    test_shot_replayer)
  add_host_test(${name})
  target_include_directories(
    ${name} PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/extraction"
  )
endforeach()

add_executable(test_robust_flow test_robust_flow.cpp)
target_compile_features(test_robust_flow PRIVATE cxx_std_17)
target_compile_options(test_robust_flow PRIVATE -Wall -Wextra)
target_include_directories(
  test_robust_flow
  PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
  "${PROJECT_SOURCE_DIR}/src/apps/extraction"
)
add_test(
  NAME test_robust_flow
  COMMAND test_robust_flow "${CMAKE_CURRENT_SOURCE_DIR}/robust_flow_reference.tsv"
)

add_test(
  NAME test_robust_flow_js
  COMMAND
    "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_robust_flow_js.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/robust_flow_reference.tsv"
    "${PROJECT_SOURCE_DIR}/web-src/app/extraction/robust-flow.js"
)

add_test(
  NAME test_flow_profile_js
  COMMAND
    "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_flow_profile_js.js"
    "${PROJECT_SOURCE_DIR}/web-src/app/extraction/robust-flow.js"
    "${PROJECT_SOURCE_DIR}/web-src/app/extraction/flow-profile.js"
)

add_test(
  NAME test_extraction_protocol_js
  COMMAND
    "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_extraction_protocol_js.js"
    "${PROJECT_SOURCE_DIR}/web-src/app/extraction/protocol.js"
)

add_test(
  NAME test_extraction_constants_js
  COMMAND
    "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_extraction_constants_js.js"
    "${PROJECT_SOURCE_DIR}/web-src/app/extraction/constants.js"
)

add_test(
  NAME test_package_m5burner
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_package_m5burner.py"
)

add_test(
  NAME test_build_web
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_build_web.py"
)

add_test(
  NAME test_license_headers
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/scripts/check_license_headers.py"
)

add_host_test(
  test_target_alert
  "${PROJECT_SOURCE_DIR}/src/apps/extraction/TargetAlert.cpp"
)
target_include_directories(
  test_target_alert PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/extraction"
)

add_host_test(test_vibration_window_trigger)
add_host_test(test_pump_decay_tracker)
add_host_test(test_pump_cluster_selector)
add_host_test(
  test_backflush_cycle
  "${PROJECT_SOURCE_DIR}/src/apps/backflush/BackflushCycle.cpp"
)

add_host_test(
  test_extraction_recorder
  "${PROJECT_SOURCE_DIR}/src/apps/extraction/ExtractionRecorder.cpp"
)
target_include_directories(
  test_extraction_recorder
  BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${PROJECT_SOURCE_DIR}/src/apps/extraction"
)

add_host_test(
  test_extraction_wire
  "${PROJECT_SOURCE_DIR}/src/apps/extraction/ExtractionWire.cpp"
  "${PROJECT_SOURCE_DIR}/src/apps/extraction/extraction_encoding.cpp"
)
target_include_directories(
  test_extraction_wire PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/extraction"
)
