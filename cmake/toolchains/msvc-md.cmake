include_guard(GLOBAL)

if (NOT WIN32)
    message(FATAL_ERROR "msvc-md.cmake is intended for Windows builds only.")
endif()

# Ensure CMAKE_MSVC_RUNTIME_LIBRARY controls /MD vs /MT.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# Dynamic CRT ABI: /MD (Release) and /MDd (Debug).
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "MSVC runtime library ABI (/MD, /MDd)"
    FORCE
)

# Keep fetched SDL's runtime ABI aligned with this toolchain.
set(SDL_FORCE_STATIC_VCRT OFF CACHE BOOL "Match SDL runtime ABI to dynamic CRT" FORCE)

# Keep vcpkg CRT linkage aligned with /MD to avoid LNK2038 runtime mismatches.
if (NOT DEFINED VCPKG_CRT_LINKAGE)
    set(VCPKG_CRT_LINKAGE "dynamic" CACHE STRING "vcpkg CRT linkage")
endif()

if (DEFINED VCPKG_CRT_LINKAGE AND NOT VCPKG_CRT_LINKAGE STREQUAL "")
    string(TOLOWER "${VCPKG_CRT_LINKAGE}" _procrogue_vcpkg_crt_lower)
    if (NOT _procrogue_vcpkg_crt_lower STREQUAL "dynamic")
        message(FATAL_ERROR
            "msvc-md.cmake expects dynamic CRT, but VCPKG_CRT_LINKAGE="
            "'${VCPKG_CRT_LINKAGE}'. Use VCPKG_CRT_LINKAGE=dynamic or switch to msvc-mt.cmake."
        )
    endif()
    unset(_procrogue_vcpkg_crt_lower)
endif()

if (DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
    string(TOLOWER "${VCPKG_TARGET_TRIPLET}" _procrogue_triplet_lower)
    if (_procrogue_triplet_lower MATCHES "-static$")
        message(FATAL_ERROR
            "msvc-md.cmake uses /MD (/MDd), but VCPKG_TARGET_TRIPLET="
            "'${VCPKG_TARGET_TRIPLET}' implies static CRT. "
            "Use a dynamic-CRT triplet (for example x64-windows or x64-windows-static-md) "
            "or switch to msvc-mt.cmake."
        )
    elseif (
        NOT _procrogue_triplet_lower MATCHES "-windows$"
        AND NOT _procrogue_triplet_lower MATCHES "-windows-static-md$"
    )
        message(STATUS
            "msvc-md.cmake: unable to infer CRT linkage from VCPKG_TARGET_TRIPLET='"
            "${VCPKG_TARGET_TRIPLET}'. Ensure it uses dynamic CRT to match /MD."
        )
    endif()
    unset(_procrogue_triplet_lower)
endif()

# Prefer a 64-bit host toolchain process when using VS generators.
if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE
        "x64"
        CACHE STRING "Host architecture for MSVC toolset"
        FORCE
    )
endif()
