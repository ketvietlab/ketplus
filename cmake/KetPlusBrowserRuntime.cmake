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

# `cmake -DKETPLUS_BROWSER_APP_DIR=<dir> -P cmake/KetPlusBrowserRuntime.cmake` prints it.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    ketplus_browser_source_digest("${KETPLUS_BROWSER_APP_DIR}" ketplus_digest_result)
    message("${ketplus_digest_result}")
endif()
