#include "../flashinfer_decl.h"

#include <flashinfer.cuh>

using namespace flashinfer;

INST_BatchPrefillRaggedWrapper(nv_bfloat16, 8, 128, true, true, QKVLayout::kHND, RotaryMode::kLlama)
