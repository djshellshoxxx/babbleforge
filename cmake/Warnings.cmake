function(bf_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
    )
  endif()
endfunction()

# Create interface target for deterministic math flags
add_library(bf_detmath_flags INTERFACE)

if(MSVC)
  target_compile_options(bf_detmath_flags INTERFACE /fp:precise)
else()
  target_compile_options(bf_detmath_flags INTERFACE -ffp-contract=off)
endif()
