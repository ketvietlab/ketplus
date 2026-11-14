# The host helper is Linux-only; keep direct/CTest invocations safe elsewhere.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
endif()
if(Python3_Interpreter_FOUND)
    add_test(NAME ketplus_ssh_install_transaction_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_ssh_install_transaction.py")
    set_tests_properties(ketplus_ssh_install_transaction_tests PROPERTIES
        SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
