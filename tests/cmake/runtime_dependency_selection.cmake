if (NOT DEFINED SOURCE_DIRECTORY OR NOT DEFINED WORK_DIRECTORY)
    message(FATAL_ERROR "SOURCE_DIRECTORY and WORK_DIRECTORY are required")
endif ()

include("${SOURCE_DIRECTORY}/cmake/HuxerUIRuntimeDependencies.cmake")

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(root "${WORK_DIRECTORY}/huxerui-runtime-selection-${nonce}")
file(MAKE_DIRECTORY
        "${root}/staging/lib" "${root}/staging/plugins" "${root}/host/lib" "${root}/staging-other")
file(WRITE "${root}/staging/lib/leaf.txt" "bundled payload")
file(WRITE "${root}/staging/plugins/leaf.txt" "replaced bundled payload")
file(WRITE "${root}/host/lib/leaf.txt" "host payload")
file(WRITE "${root}/staging-other/leaf.txt" "sibling directory payload")
set(staged_leaf "${root}/staging/lib/leaf.txt")
set(replaced_leaf "${root}/staging/plugins/leaf.txt")
set(host_leaf "${root}/host/lib/leaf.txt")
set(sibling_leaf "${root}/staging-other/leaf.txt")

function(expect_list description actual)
    set(expected ${ARGN})
    if (NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected ${description}: '${actual}' instead of '${expected}'")
    endif ()
endfunction()

# A bundled candidate wins over a same-name host copy, and the buckets stay
# available for the conflict diagnostic.
_huxerui_runtime_conflict_candidates(selected staged external "${root}/staging/"
        "${staged_leaf}" "${host_leaf}")
file(REAL_PATH "${staged_leaf}" staged_real)
file(REAL_PATH "${host_leaf}" host_real)
expect_list("only the staged candidate to be adjudicated" "${selected}" "${staged_real}")
expect_list("the staged candidate bucket" "${staged}" "${staged_real}")
expect_list("the external candidate bucket" "${external}" "${host_real}")

# Without a bundled copy the caller still validates every resolved candidate.
_huxerui_runtime_conflict_candidates(selected staged external "${root}/staging"
        "${host_leaf}" "${sibling_leaf}")
file(REAL_PATH "${sibling_leaf}" sibling_real)
expect_list("external candidates to be adjudicated" "${selected}" "${host_real}" "${sibling_real}")
expect_list("an empty staged bucket" "${staged}")
expect_list("both external candidates" "${external}" "${host_real}" "${sibling_real}")

# Two different bundled copies remain a genuine conflict for the SHA-256 check.
_huxerui_runtime_conflict_candidates(selected staged external "${root}/staging"
        "${staged_leaf}" "${replaced_leaf}")
file(REAL_PATH "${replaced_leaf}" replaced_real)
expect_list("both staged candidates to be adjudicated" "${selected}" "${staged_real}" "${replaced_real}")

# Callers that resolve without a staging tree keep the previous behavior.
_huxerui_runtime_conflict_candidates(selected staged external ""
        "${staged_leaf}" "${host_leaf}")
expect_list("every candidate to be adjudicated" "${selected}" "${staged_real}" "${host_real}")
expect_list("no staged bucket" "${staged}")
expect_list("every candidate in the external bucket" "${external}" "${staged_real}" "${host_real}")

# The root and candidate paths are normalized before the prefix comparison, and
# a sibling directory that shares the prefix text is not part of the tree.
_huxerui_runtime_conflict_candidates(selected staged external "${root}/staging/../staging"
        "${root}/staging/./lib/leaf.txt" "${sibling_leaf}")
expect_list("normalized staged candidates to be adjudicated" "${selected}" "${staged_real}")
expect_list("no external candidate for a sibling directory" "${external}" "${sibling_real}")

file(REAL_PATH "${WORK_DIRECTORY}" work_root)
file(REAL_PATH "${root}" cleanup_root)
file(RELATIVE_PATH cleanup_relative "${work_root}" "${cleanup_root}")
if (NOT cleanup_relative MATCHES "^huxerui-runtime-selection-[0-9a-f]+$")
    message(FATAL_ERROR "Refusing to clean a selection fixture outside its work directory: ${cleanup_root}")
endif ()
file(REMOVE_RECURSE "${cleanup_root}")
message(STATUS "HuxerUI runtime conflict selection fixtures passed")
