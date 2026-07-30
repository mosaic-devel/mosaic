# Translation coverage per language, for the `i18n-stats` target.
#
# Script mode (cmake -P), so it reports what is on disk right now rather than what the last
# configure saw. Invoked with -DPO_DIR=<po dir> -DMSGFMT=<msgfmt path>.

file(STRINGS "${PO_DIR}/LINGUAS" _linguas REGEX "^[^#]")
message(STATUS "")
message(STATUS "  language        translated  untranslated   coverage")
message(STATUS "  --------------- ----------  ------------   --------")

set(_total_done 0)
set(_total_all 0)
foreach(_lang IN LISTS _linguas)
    string(STRIP "${_lang}" _lang)
    set(_po "${PO_DIR}/${_lang}/mosaic.po")
    if(NOT _lang OR NOT EXISTS "${_po}")
        continue()
    endif()
    # msgfmt --statistics reports on stderr, in the form "N translated messages, M untranslated".
    execute_process(
        COMMAND "${MSGFMT}" --statistics --output-file /dev/null "${_po}"
        ERROR_VARIABLE _out OUTPUT_QUIET RESULT_VARIABLE _rc)
    set(_done 0)
    set(_todo 0)
    if(_out MATCHES "([0-9]+) translated")
        set(_done "${CMAKE_MATCH_1}")
    endif()
    if(_out MATCHES "([0-9]+) untranslated")
        set(_todo "${CMAKE_MATCH_1}")
    endif()
    math(EXPR _all "${_done} + ${_todo}")
    set(_pct 0)
    if(_all GREATER 0)
        math(EXPR _pct "(${_done} * 100) / ${_all}")
    endif()
    math(EXPR _total_done "${_total_done} + ${_done}")
    math(EXPR _total_all "${_total_all} + ${_all}")

    string(LENGTH "${_lang}" _n)
    math(EXPR _pad "15 - ${_n}")
    string(REPEAT " " ${_pad} _sp)
    message(STATUS "  ${_lang}${_sp} ${_done}\t    ${_todo}\t       ${_pct}%")
endforeach()

set(_overall 0)
if(_total_all GREATER 0)
    math(EXPR _overall "(${_total_done} * 100) / ${_total_all}")
endif()
message(STATUS "")
message(STATUS "  overall: ${_total_done}/${_total_all} strings (${_overall}%)")
message(STATUS "")
