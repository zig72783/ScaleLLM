#include "../flashinfer_decl.h"

#include <flashinfer.cuh>

using namespace flashinfer;

INST_SinglePrefill(nv_bfloat16, 1, 256, false, false, QKVLayout::kNHD, RotaryMode::kLlama)
