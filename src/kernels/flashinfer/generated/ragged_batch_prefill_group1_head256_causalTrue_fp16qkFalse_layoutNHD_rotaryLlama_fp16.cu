#include "../flashinfer_decl.h"

#include <flashinfer.cuh>

using namespace flashinfer;

INST_BatchPrefillRaggedWrapper(nv_half, 1, 256, true, false, QKVLayout::kNHD, RotaryMode::kLlama)
