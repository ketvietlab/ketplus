# Windows install tree: `cmake --install` copies the targets into <prefix>/bin, then each
# executable is run through windeployqt (Qt DLLs, plugins, the MSVC runtime) and the OpenSSL DLLs
# the daemon links are copied beside them, so the folder runs on a machine without Qt or
# Visual Studio. Nothing is copied from the build tree by hand (AGENTS.md, "Replacement bundles").
# Included from packaging/CMakeLists.txt, after every application target exists.
#
# Code signing is not done here: sign bin/*.exe and bin/*.dll of the staged tree before it is zipped
# (docs/release-process.md).

# The executables each edition builds. A target that is not part of this configuration is skipped.
set(ketplus_windows_deployed_targets ketplus ketplus_daemon ketplusctl)

foreach(ketplus_deployed_target IN LISTS ketplus_windows_deployed_targets)
    if(NOT TARGET ${ketplus_deployed_target})
        continue()
    endif()
    # The headless programs draw nothing: skip the Direct3D shader compilers windeployqt adds.
    set(ketplus_deploy_options "")
    if(ketplus_deployed_target STREQUAL "ketplus_daemon" OR ketplus_deployed_target STREQUAL "ketplusctl")
        set(ketplus_deploy_options DEPLOY_TOOL_OPTIONS
            --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw)
    endif()
    # NO_COMPILER_RUNTIME: windeployqt would copy the vc_redist installer; the runtime DLLs are
    # installed app-local below instead, so the folder needs no installer step.
    qt_generate_deploy_app_script(
        TARGET ${ketplus_deployed_target}
        OUTPUT_SCRIPT ketplus_deploy_script_${ketplus_deployed_target}
        NO_TRANSLATIONS
        NO_COMPILER_RUNTIME
        NO_UNSUPPORTED_PLATFORM_ERROR
        ${ketplus_deploy_options}
    )
    install(SCRIPT "${ketplus_deploy_script_${ketplus_deployed_target}}" COMPONENT Runtime)
endforeach()

# The MSVC runtime (vcruntime140, msvcp140, ...) beside the executables.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

# OpenSSL is linked dynamically by the daemon and by Qt's OpenSSL TLS backend. windeployqt does not
# copy it. The DLLs sit in <OpenSSL>/bin next to the import libraries' lib/ folder.
if(TARGET ketplus_daemon OR TARGET ketplus)
    set(ketplus_openssl_hints "")
    if(OPENSSL_ROOT_DIR)
        list(APPEND ketplus_openssl_hints "${OPENSSL_ROOT_DIR}/bin")
    endif()
    if(OPENSSL_CRYPTO_LIBRARY)
        get_filename_component(ketplus_openssl_lib_dir "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
        list(APPEND ketplus_openssl_hints "${ketplus_openssl_lib_dir}/../bin")
    endif()
    unset(KETPLUS_OPENSSL_CRYPTO_DLL CACHE)
    unset(KETPLUS_OPENSSL_SSL_DLL CACHE)
    find_file(KETPLUS_OPENSSL_CRYPTO_DLL NAMES libcrypto-3-x64.dll libcrypto-3.dll
        HINTS ${ketplus_openssl_hints} NO_DEFAULT_PATH)
    find_file(KETPLUS_OPENSSL_SSL_DLL NAMES libssl-3-x64.dll libssl-3.dll
        HINTS ${ketplus_openssl_hints} NO_DEFAULT_PATH)
    if(NOT KETPLUS_OPENSSL_CRYPTO_DLL OR NOT KETPLUS_OPENSSL_SSL_DLL)
        message(FATAL_ERROR
            "OpenSSL 3 runtime DLLs (libcrypto-3-x64.dll, libssl-3-x64.dll) were not found under "
            "${ketplus_openssl_hints}; set OPENSSL_ROOT_DIR to the OpenSSL install that was linked.")
    endif()
    install(FILES "${KETPLUS_OPENSSL_CRYPTO_DLL}" "${KETPLUS_OPENSSL_SSL_DLL}"
        DESTINATION bin COMPONENT Runtime)
endif()

# Qt's own third-party notices, from the SBOM beside the Qt that was deployed. Installed after the
# deploy scripts so the libraries it reads are already in the tree.
get_filename_component(ketplus_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
install(CODE "
    set(STAGE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    set(QT_SBOM_DIR \"${ketplus_qt_prefix}/sbom\")
    include(\"${CMAKE_CURRENT_LIST_DIR}/QtNotices.cmake\")
" COMPONENT Runtime)

include("${CMAKE_CURRENT_LIST_DIR}/BrowserRuntime.cmake")
