if (NOT WIN32)
    return()
endif()

if (NOT DEFINED PROCROGUE_UNLOCK_IMAGE OR PROCROGUE_UNLOCK_IMAGE STREQUAL "")
    message(FATAL_ERROR "PROCROGUE_UNLOCK_IMAGE was not provided to unlock_binary.cmake.")
endif()

execute_process(
    COMMAND taskkill /F /T /IM "${PROCROGUE_UNLOCK_IMAGE}"
    RESULT_VARIABLE _PROCROGUE_TASKKILL_RC
    OUTPUT_QUIET
    ERROR_QUIET
)

if (_PROCROGUE_TASKKILL_RC EQUAL 0)
    message(STATUS "[procrogue] Stopped running ${PROCROGUE_UNLOCK_IMAGE} before link to avoid file locks.")
endif()
