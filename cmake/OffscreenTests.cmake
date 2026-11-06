# Tests never put windows on the developer's screen: every test added in the calling directory
# draws offscreen, whatever its own ENVIRONMENT says and whatever the shell exported.
function(ketplus_tests_offscreen)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(tests)
        set_property(TEST ${tests} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "QT_QPA_PLATFORM=set:offscreen")
    endif()
endfunction()
