﻿
include(${CMAKE_CURRENT_LIST_DIR}/script_support.cmake)

set(ONIGURUMA_ROOT ${CMAKE_CURRENT_LIST_DIR}/oniguruma_${TOOLSET})
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ONIGURUMA_ROOT "${ONIGURUMA_ROOT}_x64")
endif()

#set(ONIGURUMA_DIR "${ONIGURUMA_ROOT}/lib/cmake/oniguruma")
#find_package(ONIGURUMA REQUIRED)
set(ONIGURUMA_INCLUDE_DIRS ${ONIGURUMA_ROOT}/include)
set(ONIGURUMA_LIBRARY_DIRS ${ONIGURUMA_ROOT}/lib)

if(MINGW)
  set(ONIGURUMA_LIB ${ONIGURUMA_LIBRARY_DIRS}/libonig.a)
else()
  if(IS_MULTI_CONFIG)
    set(ONIGURUMA_LIB
      debug ${ONIGURUMA_LIBRARY_DIRS}/onigd.lib
      optimized ${ONIGURUMA_LIBRARY_DIRS}/onig.lib
    )
  else()
    set(ONIGURUMA_LIB
      ${ONIGURUMA_LIBRARY_DIRS}/onig.lib
    )
  endif()
endif()
