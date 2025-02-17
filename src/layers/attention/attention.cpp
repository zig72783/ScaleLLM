#include "attention.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/torch.h>

#include "kernels/flash_attn/flash_api.h"

DEFINE_bool(disable_custom_kernels, false, "disable all custom kernels");

namespace llm {
AttentionImpl::AttentionImpl(int64_t n_heads,
                             int64_t n_kv_heads,
                             int64_t head_dim,
                             AttentionHandler* handler)
    : n_heads_(n_heads),
      n_kv_heads_(n_kv_heads),
      head_dim_(head_dim),
      handler_(handler) {
  CHECK(handler_ != nullptr);
  CHECK(n_heads % n_kv_heads == 0)
      << "n_heads " << n_heads << " not divisible by n_kv_heads " << n_kv_heads;
}

torch::Tensor AttentionImpl::forward(const torch::Tensor& query,
                                     const torch::Tensor& key,
                                     const torch::Tensor& value,
                                     KVCache& kv_cache,
                                     const InputParameters& input_params) {
  const int64_t n_tokens = query.size(0);
  // (n_tokens, n_heads, head_dim)
  auto q = query.view({n_tokens, n_heads_, head_dim_});
  auto k = key.view({n_tokens, n_kv_heads_, head_dim_});
  auto v = value.view({n_tokens, n_kv_heads_, head_dim_});

  // append key and value to kv_cache
  handler_->append_kv_cache(kv_cache, k, v, input_params);

  auto output = torch::empty_like(q);
  if (input_params.all_prefill_sequences) {
    handler_->batch_prefill(q, k, v, input_params, output);
  } else {
    handler_->batch_decode(q, kv_cache, input_params, output);
  }
  // reshape output to [n_tokens, n_heads * head_dim]
  return output.view({n_tokens, -1});
}

}  // namespace llm
