# The digest apps/browser/scripts/source-digest.mjs writes to runtime.json as sourceSha256.
function(ketplus_browser_source_digest app_dir out_var)
    file(GLOB_RECURSE ketplus_digest_files RELATIVE "${app_dir}" LIST_DIRECTORIES false
        "${app_dir}/src/*" "${app_dir}/.claude/skills/ketplus-browser/*")
    list(APPEND ketplus_digest_files package.json package-lock.json)
    list(SORT ketplus_digest_files)
    set(ketplus_digest_manifest "")
    foreach(ketplus_digest_file IN LISTS ketplus_digest_files)
        file(SHA256 "${app_dir}/${ketplus_digest_file}" ketplus_digest_hash)
        string(APPEND ketplus_digest_manifest "${ketplus_digest_file}\n${ketplus_digest_hash}\n")
    endforeach()
    string(SHA256 ketplus_digest "${ketplus_digest_manifest}")
    set(${out_var} "${ketplus_digest}" PARENT_SCOPE)
endfunction()

# Fails unless the staged runtime's sourceSha256 matches app_dir. A runtime staged from another
# commit would ship stale src/ (for example an older origin policy) without any other mismatch
# (#94). This runs at configure time and again before every bundle and install, because sources
# or the runtime can change after configure.
function(ketplus_browser_check_runtime app_dir runtime_dir allow_stale)
    if(NOT EXISTS "${runtime_dir}/runtime.json")
        message(FATAL_ERROR "The browser runtime has no runtime.json: ${runtime_dir}")
    endif()
    file(READ "${runtime_dir}/runtime.json" ketplus_runtime_json)
    string(JSON ketplus_runtime_source ERROR_VARIABLE ketplus_json_error
        GET "${ketplus_runtime_json}" sourceSha256)
    ketplus_browser_source_digest("${app_dir}" ketplus_checkout_source)
    if(ketplus_runtime_source STREQUAL ketplus_checkout_source)
        return()
    endif()
    if(allow_stale)
        message(WARNING "The browser runtime ${runtime_dir} was staged from other sources.")
    else()
        message(FATAL_ERROR
            "The browser runtime ${runtime_dir} was staged from sources "
            "'${ketplus_runtime_source}', not this checkout's '${ketplus_checkout_source}'. "
            "Re-stage it with apps/browser/scripts/package-runtime.mjs, or pass "
            "-DKETPLUS_BROWSER_RUNTIME_ALLOW_STALE=ON deliberately.")
    endif()
endfunction()

# Fails unless a prebuilt runtime is the ketplus-browser runtime for this platform and the Node.js
# version pinned in apps/browser/runtime/node-runtime.json. platform is "darwin-arm64",
# "win32-x64", ... as package-runtime.mjs writes it.
function(ketplus_browser_check_runtime_manifest app_dir runtime_dir platform)
    if(NOT EXISTS "${runtime_dir}/runtime.json")
        message(FATAL_ERROR "The browser runtime has no runtime.json: ${runtime_dir}")
    endif()
    file(READ "${app_dir}/runtime/node-runtime.json" ketplus_node_pin_json)
    string(JSON ketplus_node_pin_version GET "${ketplus_node_pin_json}" version)
    file(READ "${runtime_dir}/runtime.json" ketplus_runtime_json)
    string(JSON ketplus_runtime_name ERROR_VARIABLE ketplus_json_error
        GET "${ketplus_runtime_json}" name)
    string(JSON ketplus_runtime_node ERROR_VARIABLE ketplus_json_error
        GET "${ketplus_runtime_json}" nodeVersion)
    string(JSON ketplus_runtime_platform ERROR_VARIABLE ketplus_json_error
        GET "${ketplus_runtime_json}" platform)
    if(NOT ketplus_runtime_name STREQUAL "ketplus-browser"
       OR NOT ketplus_runtime_node STREQUAL ketplus_node_pin_version
       OR NOT ketplus_runtime_platform STREQUAL platform)
        message(FATAL_ERROR
            "The browser runtime ${runtime_dir} is '${ketplus_runtime_name}' node "
            "'${ketplus_runtime_node}' platform '${ketplus_runtime_platform}'; this build needs "
            "ketplus-browser node ${ketplus_node_pin_version} ${platform}. "
            "Re-stage it with apps/browser/scripts/package-runtime.mjs.")
    endif()
endfunction()

# Script mode, with -DKETPLUS_BROWSER_APP_DIR=<dir>:
#   alone, prints the digest;
#   with -DKETPLUS_BROWSER_RUNTIME_DIR=<dir> [-DKETPLUS_BROWSER_RUNTIME_ALLOW_STALE=ON], checks it.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(DEFINED KETPLUS_BROWSER_RUNTIME_DIR)
        ketplus_browser_check_runtime("${KETPLUS_BROWSER_APP_DIR}" "${KETPLUS_BROWSER_RUNTIME_DIR}"
            "${KETPLUS_BROWSER_RUNTIME_ALLOW_STALE}")
    else()
        ketplus_browser_source_digest("${KETPLUS_BROWSER_APP_DIR}" ketplus_digest_result)
        message("${ketplus_digest_result}")
    endif()
endif()
