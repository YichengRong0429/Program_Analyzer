# Linked into every instrumented program.
add_library(goto_runtime STATIC runtime.c)
set_target_properties(goto_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
