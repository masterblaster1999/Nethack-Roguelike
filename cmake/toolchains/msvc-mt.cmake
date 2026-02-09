include_guard(GLOBAL)

if (NOT WIN32)
    message(FATAL_ERROR "msvc-mt.cmake is intended for Windows builds only.")
endif()

# Ensure CMAKE_MSVC_RUNTIME_LIBRARY controls /MD vs /MT.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# Static CRT ABI: /MT (Release) and /MTd (Debug).
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CACHE STRING "MSVC runtime library ABI (/MT, /MTd)"
    FORCE
)

# Keep fetched SDL's runtime ABI aligned with this toolchain.
set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "Match SDL runtime ABI to static CRT" FORCE)

# Keep vcpkg CRT linkage aligned with /MT to avoid LNK2038 runtime mismatches.
if (NOT DEFINED VCPKG_CRT_LINKAGE)
    set(VCPKG_CRT_LINKAGE "static" CACHE STRING "vcpkg CRT linkage")
endif()

if (DEFINED VCPKG_CRT_LINKAGE AND NOT VCPKG_CRT_LINKAGE STREQUAL "")
    string(TOLOWER "${VCPKG_CRT_LINKAGE}" _procrogue_vcpkg_crt_lower)
    if (NOT _procrogue_vcpkg_crt_lower STREQUAL "static")
        message(FATAL_ERROR
            "msvc-mt.cmake expects static CRT, but VCPKG_CRT_LINKAGE="
            "'${VCPKG_CRT_LINKAGE}'. Use VCPKG_CRT_LINKAGE=static or switch to msvc-md.cmake."
        )
    endif()
    unset(_procrogue_vcpkg_crt_lower)
endif()

if (DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
    string(TOLOWER "${VCPKG_TARGET_TRIPLET}" _procrogue_triplet_lower)
    if (_procrogue_triplet_lower MATCHES "-static-md$" OR _procrogue_triplet_lower MATCHES "-windows$")
        message(FATAL_ERROR
            "msvc-mt.cmake uses /MT (/MTd), but VCPKG_TARGET_TRIPLET="
            "'${VCPKG_TARGET_TRIPLET}' implies dynamic CRT. "
            "Use a static-CRT triplet (for example x64-windows-static) "
            "or switch to msvc-md.cmake."
        )
    elseif (NOT _procrogue_triplet_lower MATCHES "-static$")
        message(STATUS
            "msvc-mt.cmake: unable to infer CRT linkage from VCPKG_TARGET_TRIPLET='"
            "${VCPKG_TARGET_TRIPLET}'. Ensure it uses static CRT to match /MT."
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
