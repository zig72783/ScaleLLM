#include "../flashinfer_decl.h"

#include <flashinfer.cuh>

using namespace flashinfer;

INST_SinglePrefill(nv_bfloat16, 8, 256, true, true, QKVLayout::kHND, RotaryMode::kNone)
