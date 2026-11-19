# Keep the configured appcast and the installed bundle bound to one release scope (ADR 0019).
function(ketplus_verify_update_scope feed channel arch)
    if(NOT channel MATCHES "^(staging|production)$")
        message(FATAL_ERROR "KETPLUS_UPDATE_CHANNEL must be staging or production for Sparkle updates")
    endif()
    if(NOT arch MATCHES "^(arm64|x86_64)$")
        message(FATAL_ERROR "Sparkle updates require exactly one explicit CMAKE_OSX_ARCHITECTURES value: arm64 or x86_64")
    endif()
    if(NOT feed MATCHES "^https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~-]+)*/${channel}/${arch}/appcast[.]xml$"
       OR feed MATCHES "/[.][.]?/")
        message(FATAL_ERROR "The Sparkle feed must be an HTTPS URL ending in /${channel}/${arch}/appcast.xml")
    endif()
endfunction()
