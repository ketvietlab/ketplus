# CTest labels and costs for the tests of one directory.
#
# ketplus_label_tests(<area>) gives every test the calling directory adds the label <area>
# (desktop, daemon or runner), so `ctest -L daemon` runs one area. A test listed in
# tests/test-costs.txt also gets that measured duration as its COST, which CTest uses to start
# long tests first and tools/ci/shard_tests.py uses to balance CI shards. A test whose cost
# reaches KETPLUS_SLOW_TEST_SECONDS is also labelled `slow`; `ctest -LE slow` skips them for a
# quick local pass. Like ketplus_tests_offscreen(), it runs when the directory finishes.
set(KETPLUS_TEST_COSTS_FILE "${CMAKE_CURRENT_LIST_DIR}/../tests/test-costs.txt")
set(KETPLUS_SLOW_TEST_SECONDS 30)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KETPLUS_TEST_COSTS_FILE}")

function(ketplus_label_tests area)
    # DEFER would expand ${area} when the call runs, after this function's scope is gone.
    cmake_language(EVAL CODE "cmake_language(DEFER CALL _ketplus_label_tests_now [[${area}]])")
endfunction()

function(_ketplus_label_tests_now area)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(NOT tests)
        return()
    endif()
    set_property(TEST ${tests} APPEND PROPERTY LABELS "${area}")
    file(STRINGS "${KETPLUS_TEST_COSTS_FILE}" lines REGEX "^[A-Za-z0-9_.-]+ +[0-9]+$")
    foreach(line IN LISTS lines)
        string(REGEX MATCH "^([A-Za-z0-9_.-]+) +([0-9]+)$" _ "${line}")
        set(name "${CMAKE_MATCH_1}")
        set(seconds "${CMAKE_MATCH_2}")
        if(NOT name IN_LIST tests)
            continue()
        endif()
        set_property(TEST ${name} PROPERTY COST ${seconds})
        if(seconds GREATER_EQUAL KETPLUS_SLOW_TEST_SECONDS)
            set_property(TEST ${name} APPEND PROPERTY LABELS slow)
        endif()
    endforeach()
endfunction()
