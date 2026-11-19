# The browser.v0 runtime for the Windows desktop (ADR 0009): the pinned node.exe, the apps/browser
# production dependencies and the skill, installed in lib\ketplus\ketplus-browser where
# findBundledBrowserRuntime looks beside bin\KetPlus.exe. No browser is bundled: the runtime uses
# the installed Microsoft Edge or Google Chrome, or downloads Chrome for Testing on first need,
# and reports BROWSER_UNAVAILABLE with the reason when neither works.
# Included from WindowsDeploy.cmake for the Pro desktop only.
if(NOT TARGET ketplus)
    return()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(ketplus_windows_browser_default ON)
else()
    set(ketplus_windows_browser_default OFF)
endif()
option(KETPLUS_BUNDLE_BROWSER_RUNTIME
    "Install the browser.v0 runtime in lib/ketplus/ketplus-browser"
    ${ketplus_windows_browser_default})
if(NOT KETPLUS_BUNDLE_BROWSER_RUNTIME)
    return()
endif()
set(KETPLUS_BROWSER_RUNTIME_DIR "" CACHE PATH
    "Prebuilt staged browser runtime to install instead of running package-runtime.mjs")
set(KETPLUS_NODE_CACHE "" CACHE PATH
    "Directory holding the pinned Node.js archive for package-runtime.mjs")

set(ketplus_browser_app "${PROJECT_SOURCE_DIR}/apps/browser")
include("${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake")
if(KETPLUS_BROWSER_RUNTIME_DIR)
    set(ketplus_browser_stage "${KETPLUS_BROWSER_RUNTIME_DIR}")
    ketplus_browser_check_runtime_manifest("${ketplus_browser_app}" "${ketplus_browser_stage}"
        win32-x64)
    ketplus_browser_check_runtime("${ketplus_browser_app}" "${ketplus_browser_stage}" FALSE)
else()
    find_program(KETPLUS_NODE_EXECUTABLE node
        DOC "Build-host Node.js (>= 22) used only to run package-runtime.mjs")
    if(NOT KETPLUS_NODE_EXECUTABLE)
        message(FATAL_ERROR
            "The Windows desktop package bundles the browser runtime: put node (>= 22) on PATH, set "
            "KETPLUS_BROWSER_RUNTIME_DIR to a staged runtime, or pass "
            "-DKETPLUS_BUNDLE_BROWSER_RUNTIME=OFF.")
    endif()
    set(ketplus_browser_stage "${CMAKE_BINARY_DIR}/browser-runtime/ketplus-browser")
    file(GLOB_RECURSE ketplus_browser_inputs CONFIGURE_DEPENDS
        "${ketplus_browser_app}/src/*" "${ketplus_browser_app}/.claude/skills/ketplus-browser/*")
    set(ketplus_browser_cache_argument)
    if(KETPLUS_NODE_CACHE)
        set(ketplus_browser_cache_argument --node-cache "${KETPLUS_NODE_CACHE}")
    endif()
    add_custom_command(OUTPUT "${ketplus_browser_stage}/runtime.json"
        COMMAND "${KETPLUS_NODE_EXECUTABLE}" "${ketplus_browser_app}/scripts/package-runtime.mjs"
            --out "${ketplus_browser_stage}" ${ketplus_browser_cache_argument}
        DEPENDS "${ketplus_browser_app}/scripts/package-runtime.mjs"
            "${ketplus_browser_app}/scripts/runtime-smoke.mjs"
            "${ketplus_browser_app}/runtime/node-runtime.json"
            "${ketplus_browser_app}/package.json" "${ketplus_browser_app}/package-lock.json"
            ${ketplus_browser_inputs}
        COMMENT "Staging the bundled browser runtime"
        VERBATIM)
    add_custom_target(ketplus_bundle_browser_runtime ALL
        DEPENDS "${ketplus_browser_stage}/runtime.json")
    add_dependencies(ketplus ketplus_bundle_browser_runtime)
endif()

# A runtime staged from other sources would ship a stale src/: check again at install time.
install(CODE "
    execute_process(COMMAND \"${CMAKE_COMMAND}\"
        \"-DKETPLUS_BROWSER_APP_DIR=${ketplus_browser_app}\"
        \"-DKETPLUS_BROWSER_RUNTIME_DIR=${ketplus_browser_stage}\"
        -P \"${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake\"
        RESULT_VARIABLE ketplus_browser_check_result)
    if(NOT ketplus_browser_check_result EQUAL 0)
        message(FATAL_ERROR \"The staged browser runtime does not match this checkout\")
    endif()
" COMPONENT Runtime)
install(DIRECTORY "${ketplus_browser_stage}/"
    DESTINATION lib/ketplus/ketplus-browser COMPONENT Runtime)

# The runtime's own notices also sit with the other license texts, where the audit looks.
install(DIRECTORY "${ketplus_browser_stage}/licenses/"
    DESTINATION share/licenses/ketplus/browser-runtime COMPONENT Runtime)
install(FILES "${ketplus_browser_stage}/THIRD_PARTY_NOTICES.md"
    DESTINATION share/licenses/ketplus/browser-runtime COMPONENT Runtime)
