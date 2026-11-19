# Tests never put windows on the developer's screen: every test added in the calling directory
# draws offscreen, whatever its own ENVIRONMENT says and whatever the shell exported. The
# override runs when the directory finishes, so it also covers tests added after the call.
function(ketplus_tests_offscreen)
    cmake_language(DEFER CALL _ketplus_tests_offscreen_now)
endfunction()

function(_ketplus_tests_offscreen_now)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(tests)
        set_property(TEST ${tests} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen")
    endif()
endfunction()
