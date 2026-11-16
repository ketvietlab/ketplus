function(ketplus_mac_package_architecture output)
    # An Intel cross-build on Apple Silicon must not inherit the host's arm64 name.
    if(CMAKE_OSX_ARCHITECTURES)
        list(JOIN CMAKE_OSX_ARCHITECTURES "-" architecture)
    else()
        set(architecture "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(${output} "${architecture}" PARENT_SCOPE)
endfunction()
