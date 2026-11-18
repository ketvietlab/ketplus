add_executable(ketplus_job_executor
    NativeJobControlAddress.cpp
    NativeJobExecutorMain.cpp
    NativeJobSupervisor.cpp
    NativeJobExecution.cpp
    NativeJobLaunchGate.cpp
    LinuxJobContainment.cpp
    LinuxJobIsolation.cpp
    LinuxJobSupervision.cpp
    NativeProcessIdentity.cpp
    ../../storage/BackgroundTaskOutput.cpp
)
target_include_directories(ketplus_job_executor PRIVATE ../..)
target_link_libraries(ketplus_job_executor PRIVATE Qt6::Core Qt6::Network Qt6::Sql)
if(APPLE)
    target_link_libraries(ketplus_job_executor PRIVATE proc)
endif()
if(COMMAND ketplus_enable_warnings)
    ketplus_enable_warnings(ketplus_job_executor)
endif()

# The client side of the executor, for processes other than the daemon that start and stop jobs
# through it. The daemon builds these sources itself.
add_library(ketplus_job_client STATIC
    NativeJobClient.cpp
    NativeJobRecovery.cpp
    LinuxJobContainment.cpp
    NativeProcessIdentity.cpp
    NativeJobControlAddress.cpp
    ../../storage/BackgroundTaskOutput.cpp
)
target_include_directories(ketplus_job_client PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>"
)
target_link_libraries(ketplus_job_client PUBLIC Qt6::Core Qt6::Network Qt6::Sql)
if(APPLE)
    target_link_libraries(ketplus_job_client PUBLIC proc)
endif()
if(COMMAND ketplus_enable_warnings)
    ketplus_enable_warnings(ketplus_job_client)
endif()
