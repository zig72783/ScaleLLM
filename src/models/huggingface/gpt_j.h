#pragma once

#include <torch/torch.h>

#include "layers/activation.h"
#include "layers/attention/attention_rope.h"
#include "layers/attention/handler.h"
#include "layers/embedding.h"
#include "layers/linear.h"
#include "layers/normalization.h"
#include "memory/kv_cache.h"
#include "models/input_parameters.h"
#include "models/model_args.h"
#include "models/model_registry.h"

// GPTJ model compatible with huggingface weights

namespace llm::hf {

class GPTJMLPImpl : public torch::nn::Module {
 public:
  GPTJMLPImpl(const ModelArgs& args,
              const QuantArgs& quant_args,
              const ParallelArgs& parallel_args,
              torch::ScalarType dtype,
              const torch::Device& device) {
    const int64_t hidden_size = args.hidden_size();
    const int64_t intermediate_size = args.intermediate_size();

    act_ = Activation::get_act_func(args.hidden_act(), device);
    CHECK(act_ != nullptr);

    // register the weight parameter
    fc_in_ = register_module("fc_in",
                             ColumnParallelLinear(hidden_size,
                                                  intermediate_size,
                                                  /*bias=*/true,
                                                  /*gather_output=*/false,
                                                  quant_args,
                                                  parallel_args,
                                                  dtype,
                                                  device));
    fc_out_ = register_module("fc_out",
                              RowParallelLinear(intermediate_size,
                                                hidden_size,
                                                /*bias=*/true,
                                                /*input_is_parallelized=*/true,
                                                quant_args,
                                                parallel_args,
                                                dtype,
                                                device));
  }

  torch::Tensor forward(torch::Tensor x) { return fc_out_(act_(fc_in_(x))); }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    fc_in_->load_state_dict(state_dict.select("fc_in."));
    fc_out_->load_state_dict(state_dict.select("fc_out."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    fc_in_->verify_loaded_weights(prefix + "fc_in.");
    fc_out_->verify_loaded_weights(prefix + "fc_out.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear fc_in_{nullptr};
  RowParallelLinear fc_out_{nullptr};

  ActFunc act_{nullptr};
};
TORCH_MODULE(GPTJMLP);

class GPTJAttentionImpl : public torch::nn::Module {
 public:
  GPTJAttentionImpl(const ModelArgs& args,
                    const QuantArgs& quant_args,
                    const ParallelArgs& parallel_args,
                    torch::ScalarType dtype,
                    const torch::Device& device,
                    AttentionHandler* handler) {
    const int64_t n_local_heads = args.n_heads() / parallel_args.world_size();
    const int64_t hidden_size = args.hidden_size();
    const int64_t head_dim = args.hidden_size() / args.n_heads();

    // register submodules
    qkv_proj_ = register_module("qkv_proj",
                                ColumnParallelLinear(hidden_size,
                                                     3 * hidden_size,
                                                     /*bias=*/false,
                                                     /*gather_output=*/false,
                                                     quant_args,
                                                     parallel_args,
                                                     dtype,
                                                     device));

    out_proj_ =
        register_module("out_proj",
                        RowParallelLinear(hidden_size,
                                          hidden_size,
                                          /*bias=*/false,
                                          /*input_is_parallelized=*/true,
                                          quant_args,
                                          parallel_args,
                                          dtype,
                                          device));

    // initialize attention
    atten_ = register_module("atten",
                             AttentionWithRoPE(n_local_heads,
                                               n_local_heads,
                                               head_dim,
                                               args.rotary_dim(),
                                               args.rope_scaling(),
                                               args.rope_theta(),
                                               args.max_position_embeddings(),
                                               /*interleaved=*/true,
                                               dtype,
                                               device,
                                               handler));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    // (num_tokens, dim) x (dim, n_heads * head_dim)
    // => (num_tokens, n_heads * head_dim)
    auto qkv = qkv_proj_(x).chunk(/*chunks=*/3, /*dim=*/-1);
    DCHECK_EQ(qkv.size(), 3);
    // calculate attention, output: (num_tokens, n_local_heads * head_dim)
    auto output =
        atten_(qkv[0], qkv[1], qkv[2], positions, kv_cache, input_params);
    return out_proj_(output);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    qkv_proj_->load_state_dict(state_dict, {"q_proj.", "k_proj.", "v_proj."});
    out_proj_->load_state_dict(state_dict.select("out_proj."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    qkv_proj_->verify_loaded_weights(prefix + "[q_proj,k_proj,v_proj].");
    out_proj_->verify_loaded_weights(prefix + "out_proj.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear qkv_proj_{nullptr};

  RowParallelLinear out_proj_{nullptr};

  // module members without parameters
  AttentionWithRoPE atten_{nullptr};
};
TORCH_MODULE(GPTJAttention);

class GPTJBlockImpl : public torch::nn::Module {
 public:
  GPTJBlockImpl(const ModelArgs& args,
                const QuantArgs& quant_args,
                const ParallelArgs& parallel_args,
                torch::ScalarType dtype,
                const torch::Device& device,
                AttentionHandler* handler) {
    // register submodules
    attn_ = register_module(
        "attn",
        GPTJAttention(args, quant_args, parallel_args, dtype, device, handler));
    mlp_ = register_module(
        "mlp", GPTJMLP(args, quant_args, parallel_args, dtype, device));
    ln_1_ = register_module("ln_1",
                            LayerNorm(args.hidden_size(),
                                      args.layer_norm_eps(),
                                      /*bias=*/true,
                                      dtype,
                                      device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    // x = x + attn(ln1(x)) + mlp(ln1(x))
    const auto h = ln_1_(x);
    const auto attn_output = attn_(h, positions, kv_cache, input_params);
    const auto mlp_output = mlp_(h);
    return x + attn_output + mlp_output;
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    attn_->load_state_dict(state_dict.select("attn."));
    mlp_->load_state_dict(state_dict.select("mlp."));
    ln_1_->load_state_dict(state_dict.select("ln_1."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    attn_->verify_loaded_weights(prefix + "attn.");
    mlp_->verify_loaded_weights(prefix + "mlp.");
    ln_1_->verify_loaded_weights(prefix + "ln_1.");
  }

 private:
  // parameter members, must be registered
  GPTJAttention attn_{nullptr};

  GPTJMLP mlp_{nullptr};

  LayerNorm ln_1_{nullptr};
};
TORCH_MODULE(GPTJBlock);

class GPTJModelImpl : public torch::nn::Module {
 public:
  GPTJModelImpl(const ModelArgs& args,
                const QuantArgs& quant_args,
                const ParallelArgs& parallel_args,
                torch::ScalarType dtype,
                const torch::Device& device) {
    // register submodules
    wte_ = register_module("wte",
                           ParallelEmbedding(args.vocab_size(),
                                             args.hidden_size(),
                                             parallel_args,
                                             dtype,
                                             device));

    handler_ = AttentionHandler::create(args, device);

    blocks_ = register_module("h", torch::nn::ModuleList());
    layers_.reserve(args.n_layers());
    for (int32_t i = 0; i < args.n_layers(); i++) {
      auto block = GPTJBlock(
          args, quant_args, parallel_args, dtype, device, handler_.get());
      layers_.push_back(block);
      blocks_->push_back(block);
    }
    ln_f_ = register_module("ln_f",
                            LayerNorm(args.hidden_size(),
                                      args.layer_norm_eps(),
                                      /*bias=*/true,
                                      dtype,
                                      device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = wte_(tokens);

    // TODO: set working space for attention handler
    for (size_t i = 0; i < layers_.size(); i++) {
      auto& layer = layers_[i];
      h = layer(h, positions, kv_caches[i], input_params);
    }
    return ln_f_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    wte_->load_state_dict(state_dict.select("wte."));
    // call each layer's load_state_dict function
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->load_state_dict(
          state_dict.select("h." + std::to_string(i) + "."));
    }
    ln_f_->load_state_dict(state_dict.select("ln_f."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    wte_->verify_loaded_weights(prefix + "wte.");
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->verify_loaded_weights(prefix + "h." + std::to_string(i) +
                                        ".");
    }
    ln_f_->verify_loaded_weights(prefix + "ln_f.");
  }

 private:
  // parameter members, must be registered
  ParallelEmbedding wte_{nullptr};

  // attention handler
  std::unique_ptr<AttentionHandler> handler_{nullptr};

  torch::nn::ModuleList blocks_{nullptr};
  // hold same data but different type as blocks_ to avoid type cast
  std::vector<GPTJBlock> layers_;

  LayerNorm ln_f_{nullptr};
};
TORCH_MODULE(GPTJModel);

class GPTJForCausalLMImpl : public torch::nn::Module {
 public:
  GPTJForCausalLMImpl(const ModelArgs& args,
                      const QuantArgs& quant_args,
                      const ParallelArgs& parallel_args,
                      torch::ScalarType dtype,
                      const torch::Device& device) {
    // register submodules
    transformer_ = register_module(
        "transformer",
        GPTJModel(args, quant_args, parallel_args, dtype, device));

    lm_head_ = register_module("lm_head",
                               ColumnParallelLinear(args.hidden_size(),
                                                    args.vocab_size(),
                                                    /*bias=*/true,
                                                    /*gather_output=*/true,
                                                    parallel_args,
                                                    dtype,
                                                    device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = transformer_(tokens, positions, kv_caches, input_params);
    // select last token for each sequence
    h = h.index_select(/*dim=*/0, input_params.last_token_idxes);
    return lm_head_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    transformer_->load_state_dict(state_dict.select("transformer."));
    lm_head_->load_state_dict(state_dict.select("lm_head."));
  }

  void verify_loaded_weights() const {
    transformer_->verify_loaded_weights("transformer.");
    lm_head_->verify_loaded_weights("lm_head.");
  }

 private:
  // parameter members, must be registered
  GPTJModel transformer_{nullptr};

  ColumnParallelLinear lm_head_{nullptr};
};
TORCH_MODULE(GPTJForCausalLM);

// register the model to make it available
REGISTER_CAUSAL_MODEL(gptj, GPTJForCausalLM);
// REGISTER_MODEL_ARGS_LOADER(gptj, load_gptj_model_args);
REGISTER_MODEL_ARGS(gptj, [&] {
  // example config:
  // https://huggingface.co/EleutherAI/gpt-j-6b/blob/main/config.json set
  // default values for args explicitly with values from:
  // https://github.com/huggingface/transformers/blob/main/src/transformers/models/gptj/configuration_gptj.py#L98
  LOAD_ARG_OR(model_type, "model_type", "gptj");
  LOAD_ARG_OR(dtype, "torch_dtype", "");
  LOAD_ARG_OR(vocab_size, "vocab_size", 50400);
  LOAD_ARG_OR(hidden_size, "n_embd", 4096);
  LOAD_ARG_OR(n_layers, "n_layer", 28);
  LOAD_ARG_OR(n_heads, "n_head", 16);
  LOAD_ARG_OR(rotary_dim, "rotary_dim", 64);
  LOAD_ARG_OR(hidden_act, "activation_function", "gelu_new");
  LOAD_ARG_OR(max_position_embeddings, "n_positions", 2048);
  LOAD_ARG_OR(layer_norm_eps, "layer_norm_epsilon", 1e-5);
  LOAD_ARG_OR(bos_token_id, "bos_token_id", 50256);
  LOAD_ARG_OR(eos_token_id, "eos_token_id", 50256);

  LOAD_ARG_OR_FUNC(intermediate_size, "n_inner", [&] {
    // set it to 4 times n_embd
    return args->hidden_size() * 4;
  });
});
}  // namespace llm::hf
