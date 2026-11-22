# Common compiler flags for all lanvid targets.
function(lanvid_set_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W3 /permissive- /EHsc /utf-8
      $<$<CONFIG:Release>:/O2 /Oi /Ot /Gy>
      $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Ot>)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra
      $<$<CONFIG:Release>:-O3>)
  endif()
endfunction()
