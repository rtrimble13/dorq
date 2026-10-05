# dorq_configure_target(<target>)
#
# Compiler settings for dorq's own targets. Third-party targets never get these:
# their warnings are not ours to fix, and their headers are included as SYSTEM.
function(dorq_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- $<$<BOOL:${DORQ_WARNINGS_AS_ERRORS}>:/WX>)
    return()
  endif()

  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-align
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference
            $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast>
            $<$<BOOL:${DORQ_WARNINGS_AS_ERRORS}>:-Werror>
            # Floating-point results are part of dorq's output, and output must not
            # change with the optimiser's mood (doc/adr/0001). Fusing a*b+c into one
            # FMA changes the rounding, so contraction is off on every compiler, and
            # -ffast-math must never be added.
            -ffp-contract=off)

  if(DORQ_SANITIZERS)
    list(JOIN DORQ_SANITIZERS "," _sanitizers)
    target_compile_options(${target} PRIVATE -fsanitize=${_sanitizers} -fno-omit-frame-pointer
                                             -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fsanitize=${_sanitizers})
  endif()

  # Coverage instrumentation for libFuzzer on everything a fuzz target links.
  if(DORQ_BUILD_FUZZERS)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
  endif()

  if(DORQ_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0)
    target_link_options(${target} PRIVATE --coverage)
  endif()
endfunction()
