cmake_minimum_required(VERSION 3.24)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/KetPlusProductionControl.cmake")
execute_process(COMMAND "${KETPLUS_DAEMON}" --build-info
    RESULT_VARIABLE result OUTPUT_VARIABLE info ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Daemon build-info failed: ${error}")
endif()
ketplus_verify_control_build_info("${info}" "${KETPLUS_CONTROL_URL}"
    "${KETPLUS_PRODUCTION_RELEASE}")
message(STATUS "Daemon control build-info matches the configured policy")
