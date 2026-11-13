# Writes the header OUT with the program's version banner parts:
#   SUP_VERSION     the version (-DVERSION, from CMakeLists.txt)
#   SUP_BUILD_ID    the source commit, 8 hex digits, with "-dirty" when
#                   tracked files differ from it ("unknown" without git)
#   SUP_BUILD_DATE  the commit's date, DD.MM.YYYY (the build date without git)
# It runs at every build (the build_id target), so the banner always names
# the commit the program was built from; the header is only rewritten when
# its text changes. In the AmigaOS 4 Docker build git is not run: the
# environment variables SUP_BUILD_ID and SUP_BUILD_DATE, set by
# tools/build_os4.sh, are used instead.
if(DEFINED ENV{SUP_BUILD_ID} AND NOT "$ENV{SUP_BUILD_ID}" STREQUAL "")
  set(id "$ENV{SUP_BUILD_ID}")
  set(date "$ENV{SUP_BUILD_DATE}")
else()
  execute_process(COMMAND git log -1 --format=%cd --date=format:%d.%m.%Y WORKING_DIRECTORY ${SRC}
                  OUTPUT_VARIABLE date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND git rev-parse --short=8 HEAD WORKING_DIRECTORY ${SRC}
                  OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r1 ERROR_QUIET)
  if(NOT r1 EQUAL 0 OR id STREQUAL "")
    set(id "unknown")
  else()
    execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${SRC} RESULT_VARIABLE r2 ERROR_QUIET)
    if(NOT r2 EQUAL 0)
      set(id "${id}-dirty")
    endif()
  endif()
endif()
if("${date}" STREQUAL "")
  string(TIMESTAMP date "%d.%m.%Y" UTC)
endif()
set(text "#define SUP_VERSION \"${VERSION}\"\n#define SUP_BUILD_ID \"${id}\"\n#define SUP_BUILD_DATE \"${date}\"\n")
if(EXISTS ${OUT})
  file(READ ${OUT} old)
else()
  set(old "")
endif()
if(NOT old STREQUAL text)
  file(WRITE ${OUT} "${text}")
endif()
