if (NOT HUXERUI_WIX_PAYLOAD_DIRECTORY)
    message(FATAL_ERROR "HUXERUI_WIX_PAYLOAD_DIRECTORY is required")
endif ()
if (NOT HUXERUI_WIX_PAYLOAD_NAME)
    message(FATAL_ERROR "HUXERUI_WIX_PAYLOAD_NAME is required")
endif ()
if (NOT HUXERUI_WIX_PAYLOAD_OUTPUT)
    message(FATAL_ERROR "HUXERUI_WIX_PAYLOAD_OUTPUT is required")
endif ()
if (NOT IS_DIRECTORY "${HUXERUI_WIX_PAYLOAD_DIRECTORY}")
    message(FATAL_ERROR "HuxerUI WiX payload directory does not exist: ${HUXERUI_WIX_PAYLOAD_DIRECTORY}")
endif ()

function(_huxerui_escape_wix_xml output value)
    string(REPLACE "&" "&amp;" HUXERUI_WIX_ESCAPED "${value}")
    string(REPLACE "\"" "&quot;" HUXERUI_WIX_ESCAPED "${HUXERUI_WIX_ESCAPED}")
    string(REPLACE "<" "&lt;" HUXERUI_WIX_ESCAPED "${HUXERUI_WIX_ESCAPED}")
    string(REPLACE ">" "&gt;" HUXERUI_WIX_ESCAPED "${HUXERUI_WIX_ESCAPED}")
    set(${output} "${HUXERUI_WIX_ESCAPED}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE HUXERUI_WIX_PAYLOAD_FILES
        LIST_DIRECTORIES FALSE
        RELATIVE "${HUXERUI_WIX_PAYLOAD_DIRECTORY}"
        "${HUXERUI_WIX_PAYLOAD_DIRECTORY}/*"
)
list(SORT HUXERUI_WIX_PAYLOAD_FILES)
set(HUXERUI_WIX_PAYLOAD_XML
        "<Wix xmlns=\"http://wixtoolset.org/schemas/v4/wxs\">\n  <Fragment>\n    <PayloadGroup Id=\"HuxerUIInstallerResources\">\n"
)
foreach (HUXERUI_WIX_PAYLOAD_FILE IN LISTS HUXERUI_WIX_PAYLOAD_FILES)
    set(HUXERUI_WIX_PAYLOAD_SOURCE
            "${HUXERUI_WIX_PAYLOAD_DIRECTORY}/${HUXERUI_WIX_PAYLOAD_FILE}"
    )
    set(HUXERUI_WIX_PAYLOAD_DESTINATION
            "${HUXERUI_WIX_PAYLOAD_NAME}/${HUXERUI_WIX_PAYLOAD_FILE}"
    )
    string(REPLACE "/" "\\" HUXERUI_WIX_PAYLOAD_DESTINATION
            "${HUXERUI_WIX_PAYLOAD_DESTINATION}"
    )
    _huxerui_escape_wix_xml(HUXERUI_WIX_PAYLOAD_SOURCE "${HUXERUI_WIX_PAYLOAD_SOURCE}")
    _huxerui_escape_wix_xml(HUXERUI_WIX_PAYLOAD_DESTINATION "${HUXERUI_WIX_PAYLOAD_DESTINATION}")
    string(APPEND HUXERUI_WIX_PAYLOAD_XML
            "      <Payload SourceFile=\"${HUXERUI_WIX_PAYLOAD_SOURCE}\" Name=\"${HUXERUI_WIX_PAYLOAD_DESTINATION}\" />\n"
    )
endforeach ()
string(APPEND HUXERUI_WIX_PAYLOAD_XML "    </PayloadGroup>\n  </Fragment>\n</Wix>\n")
file(WRITE "${HUXERUI_WIX_PAYLOAD_OUTPUT}" "${HUXERUI_WIX_PAYLOAD_XML}")
