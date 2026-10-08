# The pass is built as a plugin that `opt` / `clang` load at run time, so it
# does not link against LLVM libraries itself: the host tool provides them.
add_library(GotoCounter MODULE GotoCounterPass.cpp)

target_include_directories(GotoCounter SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(GotoCounter PRIVATE ${LLVM_DEFINITIONS_LIST})

# Must match how LLVM itself was built, or loading the plugin fails.
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(GotoCounter PRIVATE -fno-rtti)
endif()
