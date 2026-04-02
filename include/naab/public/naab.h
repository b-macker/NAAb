#pragma once
// NAAb Language — Public C++ Embedding API
// Include this single header to embed NAAb in a C++ application.
// Stable within minor versions (0.7.x).
//
// Quick start:
//   #include "naab/public/naab.h"
//
//   naab::Context ctx("restricted");           // isolated sandbox
//   naab::Val result = ctx.eval("main { 42 }"); // returns naab::Val(42)
//
// For CMake projects:
//   find_package(NaabLang 0.7 REQUIRED)
//   target_link_libraries(my_target NaabLang::naab_interpreter)

#include "naab/public/naab_val.h"
#include "naab/public/naab_interpreter.h"
#include "naab/public/naab_sandbox.h"

// Version constants — from configure_file(include/naab/config.h.in)
#include "naab/config.h"
