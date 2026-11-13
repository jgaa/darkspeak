# Apply only to the fetched pinned source. Never edit a sibling checkout.
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE check OUTPUT_QUIET ERROR_QUIET)
if(check EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE applied)
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR "Failed to apply pinned yahat consumer hardening")
    endif()
else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --reverse --check "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}" RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT already_applied EQUAL 0)
        message(FATAL_ERROR "yahat source does not match the pinned consumer patch")
    endif()
endif()
