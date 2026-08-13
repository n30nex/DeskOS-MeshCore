# Exact provenance substituted by `git archive` for source-only release builds.
# A normal checkout retains the literal placeholders and resolves the current
# clean HEAD instead.  Do not accept caller-supplied archive identity as proof.
set(D1L_ARCHIVE_GIT_COMMIT "$Format:%H$")
set(D1L_ARCHIVE_GIT_EPOCH_SEC "$Format:%ct$")

function(d1l_resolve_source_provenance SOURCE_ROOT OUT_COMMIT OUT_EPOCH_SEC)
    set(_d1l_ci_commit "$ENV{D1L_SOURCE_SHA}")
    set(_d1l_ci_commit_name "D1L_SOURCE_SHA")
    if("${_d1l_ci_commit}" STREQUAL "")
        set(_d1l_ci_commit "$ENV{GITHUB_SHA}")
        set(_d1l_ci_commit_name "GITHUB_SHA")
    endif()

    if(EXISTS "${SOURCE_ROOT}/.git")
        execute_process(
            COMMAND git rev-parse --verify HEAD
            WORKING_DIRECTORY "${SOURCE_ROOT}"
            RESULT_VARIABLE _d1l_head_result
            OUTPUT_VARIABLE _d1l_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE _d1l_head_error
        )
        if(NOT _d1l_head_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to resolve the current D1L checkout HEAD: ${_d1l_head_error}")
        endif()

        execute_process(
            COMMAND git status --porcelain=v1 --untracked-files=all
            WORKING_DIRECTORY "${SOURCE_ROOT}"
            RESULT_VARIABLE _d1l_status_result
            OUTPUT_VARIABLE _d1l_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE _d1l_status_error
        )
        if(NOT _d1l_status_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to verify D1L release cleanliness: ${_d1l_status_error}")
        endif()
        if(NOT "${_d1l_status}" STREQUAL "")
            message(FATAL_ERROR
                "D1L firmware provenance requires a clean current checkout")
        endif()

        if(DEFINED D1L_SOURCE_GIT_COMMIT AND
           NOT "${D1L_SOURCE_GIT_COMMIT}" STREQUAL "${_d1l_commit}")
            message(FATAL_ERROR
                "D1L source commit override does not match current HEAD")
        endif()
        if(NOT "${_d1l_ci_commit}" STREQUAL "" AND
           NOT "${_d1l_ci_commit}" STREQUAL "${_d1l_commit}")
            message(FATAL_ERROR
                "${_d1l_ci_commit_name} does not match current D1L checkout HEAD")
        endif()

        execute_process(
            COMMAND git show -s --format=%ct HEAD
            WORKING_DIRECTORY "${SOURCE_ROOT}"
            RESULT_VARIABLE _d1l_epoch_result
            OUTPUT_VARIABLE _d1l_epoch
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE _d1l_epoch_error
        )
        if(NOT _d1l_epoch_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to resolve the current D1L checkout epoch: ${_d1l_epoch_error}")
        endif()
    else()
        set(_d1l_commit "${D1L_ARCHIVE_GIT_COMMIT}")
        set(_d1l_epoch "${D1L_ARCHIVE_GIT_EPOCH_SEC}")
        if(_d1l_commit MATCHES "Format:%H" OR
           _d1l_epoch MATCHES "Format:%ct")
            message(FATAL_ERROR
                "Source-only builds require committed git-archive export-substituted provenance")
        endif()
        if(DEFINED D1L_SOURCE_GIT_COMMIT AND
           NOT "${D1L_SOURCE_GIT_COMMIT}" STREQUAL "${_d1l_commit}")
            message(FATAL_ERROR
                "D1L source commit override does not match archive provenance")
        endif()
        if(DEFINED D1L_BUILD_EPOCH_SEC AND
           NOT "${D1L_BUILD_EPOCH_SEC}" STREQUAL "${_d1l_epoch}")
            message(FATAL_ERROR
                "D1L build epoch override does not match archive provenance")
        endif()
        if(NOT "${_d1l_ci_commit}" STREQUAL "" AND
           NOT "${_d1l_ci_commit}" STREQUAL "${_d1l_commit}")
            message(FATAL_ERROR
                "${_d1l_ci_commit_name} does not match archive provenance")
        endif()
    endif()

    string(LENGTH "${_d1l_commit}" _d1l_commit_length)
    if(NOT _d1l_commit_length EQUAL 40 OR
       NOT _d1l_commit MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR
            "An exact 40-character D1L source commit is required")
    endif()
    string(TOLOWER "${_d1l_commit}" _d1l_commit)

    if(NOT _d1l_epoch MATCHES "^[0-9]+$" OR
       _d1l_epoch LESS 1767225600 OR
       _d1l_epoch GREATER 3978743295)
        message(FATAL_ERROR
            "D1L build epoch is outside the release-safe protocol range")
    endif()
    if(DEFINED D1L_BUILD_EPOCH_SEC AND
       NOT "${D1L_BUILD_EPOCH_SEC}" STREQUAL "${_d1l_epoch}")
        message(FATAL_ERROR
            "D1L build epoch does not match the exact current source")
    endif()

    set(${OUT_COMMIT} "${_d1l_commit}" PARENT_SCOPE)
    set(${OUT_EPOCH_SEC} "${_d1l_epoch}" PARENT_SCOPE)
endfunction()
