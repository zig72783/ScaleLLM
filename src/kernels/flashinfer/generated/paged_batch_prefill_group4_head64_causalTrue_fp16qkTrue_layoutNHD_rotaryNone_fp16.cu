#include "../flashinfer_decl.h"

#include <flashinfer.cuh>

using namespace flashinfer;

INST_BatchPrefillPagedWrapper(nv_half, 4, 64, true, true, QKVLayout::kNHD, RotaryMode::kNone)
