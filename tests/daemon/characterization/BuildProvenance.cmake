execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${REPO}"
  OUTPUT_VARIABLE SHA OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND git status --porcelain --untracked-files=normal WORKING_DIRECTORY "${REPO}"
  OUTPUT_VARIABLE STATUS COMMAND_ERROR_IS_FATAL ANY)
set(DIRTY 0)
if(NOT STATUS STREQUAL "")
  set(DIRTY 1)
endif()
set(FILES
  apps/daemon/src/cloud/CloudEventProjection.cpp apps/daemon/src/cloud/CloudEventProjection.h
  apps/daemon/src/storage/LocalJournal.cpp apps/daemon/src/storage/LocalJournal.h
  apps/daemon/src/storage/LocalJournalActivityGroups.cpp
  apps/daemon/src/storage/LocalJournalActivityLatest.cpp
  apps/daemon/src/storage/LocalJournalLargeEvents.cpp
  apps/daemon/src/protocol/DaemonProtocol.cpp apps/daemon/src/protocol/DaemonProtocol.h)
file(GLOB TEST_FILES RELATIVE "${REPO}" "${REPO}/tests/daemon/characterization/*")
list(APPEND FILES ${TEST_FILES})
list(SORT FILES)
set(DIGEST_INPUT "")
foreach(FILE IN LISTS FILES)
  if(NOT IS_DIRECTORY "${REPO}/${FILE}")
    file(SHA256 "${REPO}/${FILE}" HASH)
    string(APPEND DIGEST_INPUT "${FILE}:${HASH}\n")
  endif()
endforeach()
string(SHA256 DIGEST "${DIGEST_INPUT}")
set(CONTENT "#pragma once\n#define R03_BUILD_SHA \"${SHA}\"\n#define R03_SOURCE_DIRTY ${DIRTY}\n#define R03_SOURCE_DIGEST \"${DIGEST}\"\n")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" PREVIOUS)
else()
  set(PREVIOUS "")
endif()
if(NOT PREVIOUS STREQUAL CONTENT)
  file(WRITE "${OUTPUT}" "${CONTENT}")
endif()
