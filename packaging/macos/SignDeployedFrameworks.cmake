if(NOT DEFINED KETPLUS_APP_BUNDLE OR NOT IS_DIRECTORY "${KETPLUS_APP_BUNDLE}")
    message(FATAL_ERROR "KetPlus app bundle is unavailable for signing")
endif()

# macdeployqt signs the frameworks the main executable links, but a framework
# only Contents/Helpers/ketplus_daemon needs (QtWebSockets) is copied and has
# its install names rewritten without being signed again. macOS then kills the
# daemon on launch. Re-sign whatever it left invalid, then the app, whose seal
# records the nested signatures.
file(GLOB deployed_frameworks LIST_DIRECTORIES true
    "${KETPLUS_APP_BUNDLE}/Contents/Frameworks/*.framework"
    "${KETPLUS_APP_BUNDLE}/Contents/Frameworks/*.dylib"
)
# Intel linkers can leave helper executables entirely unsigned. Seal these nested
# Mach-O files before sealing the outer app; leave scripts and nested app bundles
# to their existing staging/signing rules.
file(GLOB deployed_helpers LIST_DIRECTORIES false
    "${KETPLUS_APP_BUNDLE}/Contents/Helpers/*")
foreach(helper IN LISTS deployed_helpers)
    file(READ "${helper}" helper_magic LIMIT 4 HEX)
    if(helper_magic MATCHES "^(cffaedfe|feedfacf|cefaedfe|feedface|cafebabe|bebafeca|cafebabf|bfbafeca)$")
        list(APPEND deployed_frameworks "${helper}")
    endif()
endforeach()
set(resigned FALSE)
foreach(framework IN LISTS deployed_frameworks)
    execute_process(
        COMMAND /usr/bin/codesign --verify --strict "${framework}"
        RESULT_VARIABLE verify_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(verify_result EQUAL 0)
        continue()
    endif()
    execute_process(
        COMMAND /usr/bin/codesign --force --sign - "${framework}"
        RESULT_VARIABLE sign_result
        ERROR_VARIABLE sign_error
    )
    if(NOT sign_result EQUAL 0)
        message(FATAL_ERROR "Unable to sign ${framework}: ${sign_error}")
    endif()
    message(STATUS "Re-signed ${framework}")
    set(resigned TRUE)
endforeach()

execute_process(
    COMMAND /usr/bin/codesign --verify --strict "${KETPLUS_APP_BUNDLE}"
    RESULT_VARIABLE app_verify_result OUTPUT_QUIET ERROR_QUIET)
if(resigned OR NOT app_verify_result EQUAL 0)
    execute_process(
        COMMAND /usr/bin/codesign --force --sign - "${KETPLUS_APP_BUNDLE}"
        RESULT_VARIABLE sign_result
        ERROR_VARIABLE sign_error
    )
    if(NOT sign_result EQUAL 0)
        message(FATAL_ERROR "Unable to sign ${KETPLUS_APP_BUNDLE}: ${sign_error}")
    endif()
endif()
