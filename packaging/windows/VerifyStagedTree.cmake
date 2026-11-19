# Checks a Windows install tree the way a user's machine would meet it:
#   cmake -DSTAGE=<install prefix> [-DKETPLUS_VERSION=x.y.z] -P packaging/windows/VerifyStagedTree.cmake
# Every program starts with PATH reduced to the Windows directories, so a Qt, OpenSSL or Visual
# Studio folder that happens to be on this machine's PATH cannot hide a missing DLL.
if(NOT STAGE OR NOT IS_DIRECTORY "${STAGE}")
    message(FATAL_ERROR "Pass -DSTAGE=<install prefix>")
endif()
# Programs below run from other working directories: use an absolute path.
get_filename_component(STAGE "${STAGE}" ABSOLUTE)

foreach(required
        bin/ketplus_daemon.exe bin/ketplusctl.exe bin/Qt6Core.dll bin/libcrypto-3-x64.dll
        bin/vcruntime140.dll bin/msvcp140.dll plugins/sqldrivers/qsqlite.dll
        share/licenses/ketplus/LICENSE share/licenses/ketplus/THIRD_PARTY_NOTICES.md
        lib/ketplus/claude-sidecar/index.mjs)
    if(NOT EXISTS "${STAGE}/${required}")
        message(FATAL_ERROR "Missing from the staged tree: ${required}")
    endif()
endforeach()
# Every license text the shipped files need, by name (docs/licensing.md, "Required distribution
# files"): an unrelated extra file must not make up for a missing one.
foreach(license_text OpenSSL-Apache-2.0.txt Qt-LGPL-3.0.txt GPL-3.0.txt
        Qt-Third-Party-Notices.txt Scintilla.txt Lexilla.txt KetPlus-Syntax-MPL-2.0.txt
        libvterm-MIT.txt KET-Sans-OFL-1.1.txt KET-Sans-Mono-OFL-1.1.txt KetQt-MPL-2.0.txt
        Ket-Design-System-MIT.txt Ket-Symbol-CC-BY-4.0.txt)
    if(NOT EXISTS "${STAGE}/share/licenses/ketplus/${license_text}")
        message(FATAL_ERROR "Missing license text in the staged tree: ${license_text}")
    endif()
endforeach()
# The Pro desktop carries the browser runtime (lib\ketplus\ketplus-browser) unless the package was
# configured without it. Its Node.js and npm licenses must be named in share\licenses.
set(browser_runtime "${STAGE}/lib/ketplus/ketplus-browser")
if(EXISTS "${STAGE}/bin/KetPlus.exe" AND NOT EXISTS "${browser_runtime}")
    message(STATUS "No browser runtime in the staged tree (packaged without it)")
elseif(EXISTS "${browser_runtime}")
    foreach(required bin/node.exe runtime.json src/mcp.mjs src/cli.mjs
            node_modules/playwright-core/package.json THIRD_PARTY_NOTICES.md)
        if(NOT EXISTS "${browser_runtime}/${required}")
            message(FATAL_ERROR "Missing from the browser runtime: ${required}")
        endif()
    endforeach()
    foreach(required Node.js-LICENSE THIRD_PARTY_NOTICES.md)
        if(NOT EXISTS "${STAGE}/share/licenses/ketplus/browser-runtime/${required}")
            message(FATAL_ERROR "Missing browser runtime license file: ${required}")
        endif()
    endforeach()
    file(GLOB playwright_licenses
        "${STAGE}/share/licenses/ketplus/browser-runtime/playwright-core@*")
    if(NOT playwright_licenses)
        message(FATAL_ERROR "Missing the playwright-core license text in browser-runtime")
    endif()
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
if(EXISTS "${browser_runtime}/runtime.json")
    # The bundled node.exe must start from the staged tree alone and load the browser server.
    execute_process(COMMAND "${browser_runtime}/bin/node.exe" --input-type=module -e
        "await import('./src/browser-service.mjs'); console.log(process.version)"
        WORKING_DIRECTORY "${browser_runtime}"
        OUTPUT_VARIABLE node_version RESULT_VARIABLE result ERROR_VARIABLE node_error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "The bundled browser runtime did not start (${result}): ${node_error}")
    endif()
    string(STRIP "${node_version}" node_version)
    message(STATUS "Browser runtime started with Node ${node_version}")
endif()
if(EXISTS "${STAGE}/bin/KetPlus.exe")
    # A GUI program has no console output to read: started without a display (offscreen) it must
    # still be running after a few seconds, so a missing DLL or plugin shows as an early exit.
    set(ENV{QT_QPA_PLATFORM} offscreen)
    set(ENV{KETPLUS_DISABLE_DAEMON_AUTOSTART} 1)
    execute_process(COMMAND "${STAGE}/bin/KetPlus.exe" RESULT_VARIABLE result
        OUTPUT_QUIET ERROR_VARIABLE gui_error TIMEOUT 8)
    if(NOT result MATCHES "timeout")
        message(FATAL_ERROR "KetPlus.exe did not stay up offscreen (${result}): ${gui_error}")
    endif()
    message(STATUS "KetPlus.exe stayed up offscreen")
endif()
message(STATUS "Staged tree OK: daemon ${product_version}")
