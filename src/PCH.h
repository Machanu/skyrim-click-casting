#pragma once

// CommonLibSSE-NG's own precompiled header. Its library headers (e.g. REL/Relocation.h)
// assume this prelude — which defines the `stl` namespace and pulls in <span>, <array>,
// etc. — has been force-included first.
#include <SKSE/Impl/PCH.h>

// CommonLibSSE-NG enables std::literals only inside `namespace SKSE`, but the CMake-
// generated __<target>Plugin.cpp emits the plugin declaration with "..."sv string-view
// literals at global scope. Bring the literal operators into global scope so that
// generated file compiles.
using namespace std::literals;
