cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KetPlusBackgroundOutputTests LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Core Network Sql Test)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(KETPLUS_BACKGROUND_OUTPUT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
if(APPLE AND NOT TARGET ketplus_job_executor)
    add_subdirectory("${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor" executor)
endif()
add_executable(ketplus_background_output_tests
    ../BackgroundTaskOutputTest.cpp
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskOutput.cpp"
)
target_include_directories(ketplus_background_output_tests PRIVATE
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src"
)
target_link_libraries(ketplus_background_output_tests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
if(COMMAND ketplus_enable_warnings)
    ketplus_enable_warnings(ketplus_background_output_tests)
endif()
add_test(NAME ketplus_background_output_tests COMMAND ketplus_background_output_tests)

add_executable(ketplus_background_ledger_tests
    ../BackgroundTaskLedgerTest.cpp
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskLedger.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskTransitions.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskDelivery.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskWakeQueue.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskQuota.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskList.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskFingerprint.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskRetention.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskExecutors.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskChildren.cpp"
)
target_include_directories(ketplus_background_ledger_tests PRIVATE
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src"
)
target_link_libraries(ketplus_background_ledger_tests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
if(COMMAND ketplus_enable_warnings)
    ketplus_enable_warnings(ketplus_background_ledger_tests)
endif()
add_test(NAME ketplus_background_ledger_tests COMMAND ketplus_background_ledger_tests)

add_executable(ketplus_managed_task_spec_tests
    ../ManagedTaskSpecTest.cpp
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskFingerprint.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/ManagedTaskSpec.cpp")
target_include_directories(ketplus_managed_task_spec_tests PRIVATE
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src")
target_link_libraries(ketplus_managed_task_spec_tests PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME ketplus_managed_task_spec_tests COMMAND ketplus_managed_task_spec_tests)

if(APPLE)
    add_executable(ketplus_native_job_workload ../NativeJobWorkload.cpp)
    add_executable(ketplus_native_job_executor_tests
        ../NativeJobExecutorTest.cpp
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobControlAddress.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeProcessIdentity.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobClient.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobRecovery.cpp"
    )
    target_include_directories(ketplus_native_job_executor_tests PRIVATE
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src")
    target_link_libraries(ketplus_native_job_executor_tests PRIVATE Qt6::Core Qt6::Network Qt6::Test proc)
    target_compile_definitions(ketplus_native_job_executor_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>"
        KETPLUS_JOB_WORKLOAD_PATH="$<TARGET_FILE:ketplus_native_job_workload>")
    add_dependencies(ketplus_native_job_executor_tests ketplus_job_executor ketplus_native_job_workload)
    add_test(NAME ketplus_native_job_executor_tests COMMAND ketplus_native_job_executor_tests)
    add_executable(ketplus_managed_task_runtime_tests
        ../ManagedTaskRuntimeTest.cpp
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobControlAddress.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/ManagedTaskRuntime.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/ManagedTaskProcesses.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/ManagedTaskOutput.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobClient.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeJobRecovery.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/runtime/executor/NativeProcessIdentity.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskLedger.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskTransitions.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskDelivery.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskWakeQueue.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskFingerprint.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskRetention.cpp"
    "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskExecutors.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskChildren.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskQuota.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskList.cpp"
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src/storage/BackgroundTaskOutput.cpp")
    target_include_directories(ketplus_managed_task_runtime_tests PRIVATE
        "${KETPLUS_BACKGROUND_OUTPUT_ROOT}/apps/daemon/src")
    target_link_libraries(ketplus_managed_task_runtime_tests PRIVATE Qt6::Core Qt6::Network Qt6::Sql Qt6::Test proc)
    target_compile_definitions(ketplus_managed_task_runtime_tests PRIVATE
        KETPLUS_JOB_EXECUTOR_PATH="$<TARGET_FILE:ketplus_job_executor>")
    add_dependencies(ketplus_managed_task_runtime_tests ketplus_job_executor)
    add_test(NAME ketplus_managed_task_runtime_tests COMMAND ketplus_managed_task_runtime_tests)
endif()
