add_executable(ketplus_job_executor
    NativeJobExecutorMain.cpp
    NativeJobSupervisor.cpp
    NativeJobExecution.cpp
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
