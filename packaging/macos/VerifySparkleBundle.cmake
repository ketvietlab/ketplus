# ADR 0019: a build configured for updates must carry Sparkle and the exact feed and key it
# was configured with; an unconfigured build must not carry either.
if(NOT DEFINED KETPLUS_UPDATES_ENABLED)
    return()
endif()
set(sparkle_plist "${KETPLUS_APP_BUNDLE}/Contents/Info.plist")
set(sparkle_framework "${KETPLUS_APP_BUNDLE}/Contents/Frameworks/Sparkle.framework")
foreach(sparkle_key IN ITEMS SUFeedURL SUPublicEDKey)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :${sparkle_key}"
            "${sparkle_plist}"
        RESULT_VARIABLE sparkle_key_result
        OUTPUT_VARIABLE sparkle_key_value
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(sparkle_plist_${sparkle_key} "")
    if(sparkle_key_result EQUAL 0)
        set(sparkle_plist_${sparkle_key} "${sparkle_key_value}")
    endif()
endforeach()
if(KETPLUS_UPDATES_ENABLED)
    if(NOT EXISTS "${sparkle_framework}/Versions/B/Sparkle"
       OR NOT IS_SYMLINK "${sparkle_framework}/Sparkle")
        message(FATAL_ERROR "KetPlus.app is missing an intact Contents/Frameworks/Sparkle.framework")
    endif()
    if(NOT sparkle_plist_SUFeedURL STREQUAL KETPLUS_UPDATE_FEED_URL
       OR NOT sparkle_plist_SUPublicEDKey STREQUAL KETPLUS_UPDATE_PUBLIC_ED_KEY)
        message(FATAL_ERROR "KetPlus.app Info.plist does not carry the configured update feed and key")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/KetPlusUpdateScope.cmake")
    ketplus_verify_update_scope("${sparkle_plist_SUFeedURL}" "${KETPLUS_UPDATE_CHANNEL}"
        "${KETPLUS_UPDATE_ARCH}")
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "${sparkle_plist}"
        RESULT_VARIABLE sparkle_executable_result
        OUTPUT_VARIABLE sparkle_executable
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT sparkle_executable_result STREQUAL "0" OR NOT sparkle_executable MATCHES "^[A-Za-z0-9_-][A-Za-z0-9._-]*$")
        message(FATAL_ERROR "KetPlus.app Info.plist does not carry a valid CFBundleExecutable")
    endif()
    execute_process(
        COMMAND /usr/bin/lipo -archs "${KETPLUS_APP_BUNDLE}/Contents/MacOS/${sparkle_executable}"
        RESULT_VARIABLE sparkle_arch_result
        OUTPUT_VARIABLE sparkle_arch
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT sparkle_arch_result STREQUAL "0" OR NOT sparkle_arch STREQUAL KETPLUS_UPDATE_ARCH)
        message(FATAL_ERROR "KetPlus.app executable must contain only ${KETPLUS_UPDATE_ARCH} for its Sparkle feed")
    endif()
elseif(EXISTS "${sparkle_framework}" OR NOT sparkle_plist_SUFeedURL STREQUAL ""
       OR NOT sparkle_plist_SUPublicEDKey STREQUAL "")
    message(FATAL_ERROR "KetPlus.app carries Sparkle although updates are not configured")
endif()
