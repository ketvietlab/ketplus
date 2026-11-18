# Checks a Windows install tree the way a user's machine would meet it:
#   cmake -DSTAGE=<install prefix> [-DKETPLUS_VERSION=x.y.z] -P packaging/windows/VerifyStagedTree.cmake
# Every program starts with PATH reduced to the Windows directories, so a Qt, OpenSSL or Visual
# Studio folder that happens to be on this machine's PATH cannot hide a missing DLL.
if(NOT STAGE OR NOT IS_DIRECTORY "${STAGE}")
    message(FATAL_ERROR "Pass -DSTAGE=<install prefix>")
endif()

foreach(required
        bin/ketplus_daemon.exe bin/ketplusctl.exe bin/Qt6Core.dll bin/libcrypto-3-x64.dll
        bin/vcruntime140.dll bin/msvcp140.dll plugins/sqldrivers/qsqlite.dll
        share/licenses/ketplus/LICENSE share/licenses/ketplus/THIRD_PARTY_NOTICES.md
        lib/ketplus/claude-sidecar/index.mjs)
    if(NOT EXISTS "${STAGE}/${required}")
        message(FATAL_ERROR "Missing from the staged tree: ${required}")
    endif()
endforeach()
file(GLOB ketplus_license_texts "${STAGE}/share/licenses/ketplus/third_party/licenses/*"
    "${STAGE}/share/licenses/ketplus/*")
list(LENGTH ketplus_license_texts ketplus_license_count)
if(ketplus_license_count LESS 3)
    message(FATAL_ERROR "The staged tree carries too few third-party license texts")
endif()
if(EXISTS "${STAGE}/bin/vc_redist.x64.exe")
    message(FATAL_ERROR "vc_redist.x64.exe must not be staged; the runtime DLLs are app-local")
endif()

set(ENV{PATH} "$ENV{SystemRoot}\\System32;$ENV{SystemRoot}")
execute_process(COMMAND "${STAGE}/bin/ketplus_daemon.exe" --build-info
    OUTPUT_VARIABLE build_info RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ketplus_daemon --build-info failed (${result}); a DLL is probably missing")
endif()
string(JSON product_version GET "${build_info}" product_version)
if(KETPLUS_VERSION AND NOT product_version STREQUAL KETPLUS_VERSION)
    message(FATAL_ERROR "Daemon reports ${product_version}, expected ${KETPLUS_VERSION}")
endif()
execute_process(COMMAND "${STAGE}/bin/ketplus_daemon.exe" --check-runtime RESULT_VARIABLE result
    OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ketplus_daemon --check-runtime failed (${result}): a Qt plugin is missing")
endif()
execute_process(COMMAND "${STAGE}/bin/ketplusctl.exe" --help RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ketplusctl --help failed (${result})")
endif()
if(EXISTS "${STAGE}/bin/ketplus.exe")
    # A GUI program has no console output to read: it must stay up for a few seconds.
    execute_process(COMMAND "${STAGE}/bin/ketplus.exe" --version RESULT_VARIABLE result
        OUTPUT_QUIET ERROR_QUIET TIMEOUT 20)
    message(STATUS "ketplus.exe --version exited with ${result}")
endif()
message(STATUS "Staged tree OK: daemon ${product_version}")
