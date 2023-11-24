//  Copyright (c) 2023 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdlib.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "common.h"
#include "core/layers/jblas_common.hpp"
#include "models/model_utils/model_types.h"
#include "models/model_utils/model_config.h"
#include "models/model_utils/model_types.h"
#include "models/model_utils/model_utils.h"

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <signal.h>
#include <windows.h>
#endif

namespace py = pybind11;

std::shared_ptr<quant_layer_base> get_model_quant_layer(const std::string model_name) {
  return ql_registry::create_ql(model_name);
}

class Model {
 public:
  Model() { model_init_backend(); }
  ~Model() {
    if (ctx) model_free(ctx);
  }
  void init_model(const std::string& model_path, int n_predict, int n_batch, int ctx_size, int seed, int threads,
                  float repetition_penalty, int num_beams, bool do_sample, int top_k, float top_p, float temperature,
                  int min_new_tokens, float length_penalty, bool early_stopping, int n_keep, int n_discard,
                  bool shift_roped_k, int batch_size, model_vocab::id pad_token, const std::string& memory_dtype);
  void reinit();
  std::vector<std::vector<model_token>> generate(const std::vector<std::vector<model_token>>& input_ids);
  std::vector<std::vector<model_token>> generate_tokens(const std::vector<std::vector<model_token>>& input_ids);
  const std::vector<float>& evaluate_(const std::vector<std::vector<model_token>>& input_ids);
  py::array_t<float> evaluate(const std::vector<std::vector<model_token>>& input_ids) {
    if (input_ids.size() != 1) {
      fprintf(stderr, "\nERROR: only support batch == 1 input!\n");
      return py::array_t<float>();
    }
    const auto& logits = evaluate_(input_ids);
    return py::array_t<float, py::array::c_style>(logits.size(), logits.data())
        .reshape({py::ssize_t(-1), static_cast<py::ssize_t>(ctx->model.hparams.n_vocab)});
  }
  bool is_token_end() { return token_eos; }
  static int quant_model(const std::string& model_path, const std::string& out_path, const std::string& weight_dtype,
                         const std::string& alg, int group_size, const std::string& scale_dtype,
                         const std::string& compute_dtype, bool use_ggml, int threads);
  void reset_token_end() {
    token_eos = false;
    curr_input_ids.clear();
    generate_count = 0;
  }

  static size_t jblas_qpack(const int8_t* src_w, const float* src_scales, const int8_t* src_zps, void* dstpr,
                            const quant_params_internal params, int nthread, int n, int k);
  static size_t jblas_quantize(const float* src_w, void* dstpr, const quant_params_internal params, int nthread, int n,
                               int k);
  static size_t np_jblas_qpack(py::array_t<int8_t> src_w, py::array_t<float> src_scales, py::array_t<int8_t> dst) {
    int8_t* w_ptr = src_w.mutable_data();
    float* scales_ptr = src_scales.mutable_data();
    int8_t* dst_ptr = dst.mutable_data();

    quant_params_internal q_params;
    q_params.bits = quant_bits::q4;
    q_params.scale_dtype = quant_sdtype::fp32;
    q_params.compute_dtype = quant_comp::int8;
    q_params.group_size = 128;
    return Model::jblas_qpack(w_ptr, scales_ptr, nullptr, dst_ptr, q_params, 1, src_w.shape(0), src_w.shape(1));
  }

  static size_t np_jblas_quantize(py::array_t<float> src_w, py::array_t<int8_t> dst) {
    quant_params_internal q_params;
    q_params.bits = quant_bits::q4;
    q_params.scale_dtype = quant_sdtype::fp32;
    q_params.compute_dtype = quant_comp::int8;
    q_params.group_size = 32;
    return Model::jblas_quantize(src_w.mutable_data(), dst.mutable_data(), q_params, 8, src_w.shape(0), src_w.shape(1));
  }

 private:
  model_context* ctx = nullptr;
  gpt_params params;
  std::vector<model_token> curr_input_ids;
  int n_past = 0;
  int n_total = 0;
  int n_vocab = 0;
  int n_ctx = 0;
  std::vector<model_token> last_n_tokens;
  bool token_eos = false;
  long int generate_count = 0;

  std::vector<std::vector<model_token>> beam_generate(const std::vector<std::vector<model_token>>& input_ids);
  model_token post_process(const float* logits);
  model_token post_greedy_search(const float* logits);
  std::vector<std::vector<model_token>> post_beam_search(model_context* lctx, const int& n_predict,
                                                         const std::vector<model_input>& inputs, const int& n_threads);
  model_token post_sample_top_k_top_p_repeat(const float* logits);
};

void Model::init_model(const std::string& model_path, int max_new_tokens, int n_batch, int ctx_size, int seed,
                       int threads, float repetition_penalty, int num_beams, bool do_sample, int top_k, float top_p,
                       float temperature, int min_new_tokens, float length_penalty, bool early_stopping, int n_keep,
                       int n_discard, bool shift_roped_k, int batch_size, model_vocab::id pad_token,
                       const std::string& memory_dtype) {
#ifdef MODEL_NAME
  params.model_name = MODEL_NAME;
#endif
  params.model_arch = model_name_to_arch::init().find(params.model_name);
  params.model = model_path;
  params.n_predict = max_new_tokens;
  params.n_batch = n_batch;
  params.n_ctx = ctx_size;
  params.seed = seed;
  params.n_threads = threads;
  params.repeat_penalty = repetition_penalty;
  params.beam_size = num_beams;
  params.do_sample = do_sample;
  params.batch_size = batch_size;
  params.beam_search = (num_beams > 1 && !do_sample) ? true : false;
  params.top_k = top_k;
  params.top_p = top_p;
  params.temp = temperature;
  params.n_keep = n_keep;
  params.n_discard = n_discard;
  params.shift_roped_k = shift_roped_k;
  if (memory_dtype == "f32")
    params.memory_type = KV_MEM_TYPE_F32;
  else if (memory_dtype == "f16")
    params.memory_type = KV_MEM_TYPE_F16;
  else if (memory_dtype == "auto")
    params.memory_type = KV_MEM_TYPE_AUTO;
  else
    fprintf(stderr, "Unexpected memory dtype!");
  if (params.beam_search) params.memory_type = KV_MEM_TYPE_F16;  // TODO(Yi): NO MHA IN BEAM SEARCH

  printf("beam_size: %d, do_sample: %d, top_k: %d, top_p: %f\n", params.beam_size, params.do_sample, params.top_k,
         params.top_p);

  n_past = 0;
  n_total = 0;
  token_eos = false;
  curr_input_ids.clear();
  ctx = model_init_from_gpt_params(params);
  n_vocab = model_n_vocab(ctx);
  n_ctx = model_n_ctx(ctx);
  last_n_tokens.resize(n_ctx, 0);
  ctx->generation_conf.min_new_tokens = min_new_tokens;
  ctx->generation_conf.length_penalty = length_penalty;
  ctx->generation_conf.do_early_stopping = early_stopping;
  if (pad_token != -1) ctx->vocab.pad_token_id = pad_token;
}

void Model::reinit() {
  n_past = 0;
  n_total = 0;
  last_n_tokens.clear();
  last_n_tokens.resize(n_ctx, 0);
  token_eos = false;
  curr_input_ids.clear();
  ctx->n_sample = 0;
  ctx->t_sample_us = 0;
  generate_count = 0;
}

std::vector<std::vector<model_token>> Model::beam_generate(const std::vector<std::vector<model_token>>& input_ids) {
  MODEL_ASSERT(input_ids.size() == ctx->batch_size);
  if (ctx->batch_size > 1 && ctx->vocab.pad_token_id == -1) {
    fprintf(stderr, "\nERROR: please set pad_token for beam search multi-batch generation!\n");
    return {};
  }
  std::vector<model_input> inputs;
  for (int bs = 0; bs < input_ids.size(); ++bs) {
    uint32_t count = 0;
    model_vocab::id pad_token_id = ctx->vocab.pad_token_id;
    auto iter = std::find_if(input_ids[bs].begin(), input_ids[bs].end(),
                             [&pad_token_id](model_token t) { return (t != pad_token_id); });
    if (iter == input_ids[bs].end()) fprintf(stderr, "\nERROR: there are all pad tokens in batch %d!\n", bs);
    count = std::distance(input_ids[bs].begin(), iter);
    inputs.push_back(model_input{
        /*.tokens              =*/input_ids[bs].data(),
        /*.n_tokens           =*/(uint32_t)input_ids[bs].size(),
        /*.n_prompt_tokens    =*/0,
        /*.n_past             =*/0,
        /*.n_total            =*/0,
        /*.request_idx        =*/bs,
        /*.beam_idx           =*/0,
        /*.padding_side       =*/0,
        /*n_padding           =*/count,
    });
  }
  return post_beam_search(ctx, params.n_predict, inputs, params.n_threads);
}

const std::vector<float>& Model::evaluate_(const std::vector<std::vector<model_token>>& input_ids) {
  static const std::vector<float> empty_ret{};
  if (input_ids.size() > 1) {
    fprintf(stderr, "\nERROR: Only beam search supports multi-batch generation!\n");
    return empty_ret;
  }

  static const std::vector<model_token> empty_id{};
  const auto& input_id0 = input_ids.empty() ? empty_id : input_ids[0];  // currently only support single batch
  if (input_id0.empty()) {                                              // use internel input id
    if (curr_input_ids.empty()) {
      fprintf(stderr, "%s: error: no input\n", __func__);
      return empty_ret;
    }
  } else if (!curr_input_ids.empty()) {
    fprintf(stderr, "%s: error: prompt confliction\n", __func__);
    return empty_ret;
  } else if (input_id0.size() > n_ctx - 4) {  // long input_id0 and empty curr_input_ids
    fprintf(stderr, "\n%s: Warning: prompt is too long (%d tokens, max %d), will be truncated\n", __func__,
            input_id0.size(), n_ctx - 4);
    curr_input_ids.resize(n_ctx - 4);
    std::copy(input_id0.end() - n_ctx - 4, input_id0.end(), curr_input_ids.begin());
  } else {  // good input_id0 and empty curr_input_ids
    curr_input_ids = input_id0;
  }

  // push elements in curr_input_ids to the last_n_tokens queue
  last_n_tokens.erase(last_n_tokens.begin(), last_n_tokens.begin() + curr_input_ids.size());
  last_n_tokens.insert(last_n_tokens.end(), curr_input_ids.begin(), curr_input_ids.end());

  // infinite text generation via context swapping
  if (n_past + curr_input_ids.size() > n_ctx) {
    // always keep the first token
    n_past = std::max(1, params.n_keep);

    int n_discard = params.n_discard;
    if (!params.shift_roped_k) {  // shift_roped_k can use ring-buffer and thus does not need re-computing
      if (n_discard == -1) n_discard = (n_ctx - curr_input_ids.size() - params.n_keep) / 2;
      // drop n_discard tokens
      curr_input_ids.insert(curr_input_ids.begin(), last_n_tokens.begin() + params.n_keep + n_discard,
                            last_n_tokens.end() - curr_input_ids.size());
    } else {
      NE_ASSERT(("n_discard cannot be used with shift_roped_k!", n_discard == -1 || n_discard == 1));
    }
  }

  std::vector<model_input> inputs{{
      /*.tokens              =*/curr_input_ids.data(),
      /*.n_tokens           =*/(uint32_t)curr_input_ids.size(),
      /*.n_prompt_tokens    =*/0,
      /*.n_past             =*/(uint32_t)n_past,
      /*.n_total            =*/(uint32_t)n_total,
      /*.request_idx        =*/0,
      /*.beam_idx           =*/0,
      /*.padding_side       =*/0,
      /*n_padding           =*/0,
  }};
  model_eval(ctx, inputs.data(), inputs.size(), params.n_threads);
  n_past += curr_input_ids.size();
  n_total += curr_input_ids.size();

  curr_input_ids.clear();  // add new tok to curr_input_ids if necessary after post processing
  return ctx->logits;
}

std::vector<std::vector<model_token>> Model::generate(const std::vector<std::vector<model_token>>& input_ids) {
  if (ctx->beam_search) return beam_generate(input_ids);
  if (input_ids.size() > 1) {
    fprintf(stderr, "\nERROR: Only beam search supports multi-batch generation!\n");
    return {};
  }

  const auto& logits = evaluate_(input_ids);
  if (logits.empty()) return {};

  model_token next_token_id = post_process(logits.data());
  curr_input_ids = {next_token_id};
  generate_count++;
  return {{next_token_id}};
}

std::vector<std::vector<model_token>> Model::generate_tokens(const std::vector<std::vector<model_token>>& input_ids) {
  int n_remain = params.n_predict;
  std::vector<model_token> output_ids;
  std::vector<std::vector<model_token>> rets;

  if (ctx->beam_search) {
    MODEL_ASSERT(input_ids.size() == ctx->batch_size);
    if (ctx->batch_size > 1 && ctx->vocab.pad_token_id == -1) {
      fprintf(stderr, "\nERROR: please set pad_token for beam search multi-batch generation!\n");
      return rets;
    }
    std::vector<model_input> inputs;
    for (int bs = 0; bs < input_ids.size(); ++bs) {
      uint32_t count = 0;
      model_vocab::id pad_token_id = ctx->vocab.pad_token_id;
      auto iter = std::find_if(input_ids[bs].begin(), input_ids[bs].end(),
                               [&pad_token_id](model_token t) { return (t != pad_token_id); });
      if (iter == input_ids[bs].end()) fprintf(stderr, "\nERROR: there are all pad tokens in batch %d!\n", bs);
      count = std::distance(input_ids[bs].begin(), iter);
      inputs.push_back(model_input{
          /*.tokens              =*/input_ids[bs].data(),
          /*.n_tokens           =*/(uint32_t)input_ids[bs].size(),
          /*.n_prompt_tokens    =*/0,
          /*.n_past             =*/0,
          /*.n_total            =*/0,
          /*.request_idx        =*/bs,
          /*.beam_idx           =*/0,
          /*.padding_side       =*/0,
          /*n_padding           =*/count,
      });
    }
    return post_beam_search(ctx, n_remain, inputs, params.n_threads);
  }
  if (input_ids.size() > 1) {
    fprintf(stderr, "\nERROR: Only beam search supports multi-batch generation!\n");
    return rets;
  }

  if (curr_input_ids.empty()) {
    if (input_ids[0].size() > n_ctx - 4) {
      fprintf(stderr, "\n%s: Warning: prompt is too long (%d tokens, max %d), will be truncated\n", __func__,
              input_ids[0].size(), n_ctx - 4);
      curr_input_ids.resize(n_ctx - 4);
      std::copy(input_ids[0].end() - n_ctx - 4, input_ids[0].end(), curr_input_ids.begin());
    } else {
      curr_input_ids = input_ids[0];
    }
  }

  while (output_ids.size() < n_remain) {
    for (auto item : curr_input_ids) {
      last_n_tokens.erase(last_n_tokens.begin());
      last_n_tokens.push_back(item);
    }
    // infinite text generation via context swapping
    if (n_past + curr_input_ids.size() > n_ctx) {
      // always keep the first token
      n_past = std::max(1, params.n_keep);

      int n_discard = params.n_discard;
      if (!params.shift_roped_k) {  // shift_roped_k can use ring-buffer and thus does not need re-computing
        if (n_discard == -1) n_discard = (n_ctx - curr_input_ids.size() - params.n_keep) / 2;
        // drop n_discard tokens
        curr_input_ids.insert(curr_input_ids.begin(), last_n_tokens.begin() + params.n_keep + n_discard,
                              last_n_tokens.end() - curr_input_ids.size());
      } else {
        NE_ASSERT(("n_discard cannot be used with shift_roped_k!", n_discard == -1 || n_discard == 1));
      }
    }
    std::vector<model_input> inputs = {model_input{
        /*.tokens              =*/curr_input_ids.data(),
        /*.n_tokens           =*/(uint32_t)curr_input_ids.size(),
        /*.n_prompt_tokens    =*/0,
        /*.n_past             =*/(uint32_t)n_past,
        /*.n_total            =*/(uint32_t)n_total,
        /*.request_idx        =*/0,
        /*.beam_idx           =*/0,
        /*.padding_side       =*/0,
        /*n_padding           =*/0,
    }};
    model_eval(ctx, inputs.data(), inputs.size(), params.n_threads);
    n_past += curr_input_ids.size();
    n_total += curr_input_ids.size();

    float* logits = model_get_logits(ctx);
    model_token next_token_id = post_process(logits);
    curr_input_ids = {next_token_id};
    output_ids.push_back(next_token_id);
    generate_count++;
    if (next_token_id == ctx->vocab.eos_token_id) {
      token_eos = true;
      break;
    }
    if (params.n_predict > 0 && generate_count >= params.n_predict) {
      token_eos = true;
      break;
    }
  }
  rets.push_back(output_ids);
  return rets;
}

model_token Model::post_greedy_search(const float* logits) {
  model_token id = std::max_element(logits, logits + n_vocab) - logits;
  return id;
}

std::vector<std::vector<model_token>> Model::post_beam_search(model_context* lctx, const int& n_predict,
                                                              const std::vector<model_input>& inputs,
                                                              const int& n_threads) {
  // TODO: to implement
  static std::set<model_archs> supported_archs = {MODEL_GPTJ, MODEL_GPTNEOX};
  if (supported_archs.count(params.model_arch) != 0) {
    return beam_search(lctx, n_predict, inputs, n_threads);
  } else {
    fprintf(stderr, "\nERROR: this model does not support beam search generation!\n");
    return std::vector<std::vector<model_token>>();
  }
}

model_token Model::post_sample_top_k_top_p_repeat(const float* logits) {
  int alpha_frequency = 0;
  int alpha_presence = 0;
  int repeat_last_n = 64;
  int top_k = params.top_k;
  float tfs_z = 1.00f;
  float typical_p = 1.00f;
  float top_p = params.top_p;
  float temp = params.temp;
  std::vector<model_token_data> candidates;
  candidates.reserve(n_vocab);
  for (model_token token_id = 0; token_id < n_vocab; token_id++) {
    candidates.emplace_back(model_token_data{token_id, logits[token_id], 0.0f});
  }
  model_token_data_array candidates_p = {candidates.data(), candidates.size(), false};

  // Apply penalties
  float nl_logit = logits[model_token_nl()];
  auto last_n_repeat = std::min(std::min((int)last_n_tokens.size(), repeat_last_n), n_ctx);
  model_sample_repetition_penalty(ctx, &candidates_p, last_n_tokens.data() + last_n_tokens.size() - last_n_repeat,
                                  last_n_repeat, params.repeat_penalty);
  model_sample_frequency_and_presence_penalties(ctx, &candidates_p,
                                                last_n_tokens.data() + last_n_tokens.size() - last_n_repeat,
                                                last_n_repeat, alpha_frequency, alpha_presence);
  // int id = model_sample_token_greedy(ctx, &candidates_p);
  // Temperature sampling
  model_sample_top_k(ctx, &candidates_p, top_k, 1);
  model_sample_tail_free(ctx, &candidates_p, tfs_z, 1);
  model_sample_typical(ctx, &candidates_p, typical_p, 1);
  model_sample_top_p(ctx, &candidates_p, top_p, 1);
  model_sample_temperature(ctx, &candidates_p, temp);
  int id = model_sample_token(ctx, &candidates_p);
  return id;
}

model_token Model::post_process(const float* logits) {
  assert(("Beam search does not support streaming.", params.beam_size == 1));
  if (params.do_sample == false) {
    return post_greedy_search(logits);
  } else {
    return post_sample_top_k_top_p_repeat(logits);
  }
}

int Model::quant_model(const std::string& model_path, const std::string& out_path, const std::string& weight_dtype,
                       const std::string& alg, int group_size, const std::string& scale_dtype,
                       const std::string& compute_dtype, bool use_ggml, int threads) {
  quant_params q_params;
#ifdef MODEL_NAME
  q_params.model_name = MODEL_NAME;
#endif
  model_archs mt = model_name_to_arch::init().find(q_params.model_name);
  if (mt == MODEL_UNKNOWN) {
    fprintf(stderr, "error, please set model_name \n");
    exit(0);
  }
  q_params.model_arch = mt;
  q_params.model_file = model_path;
  q_params.out_file = out_path;
  q_params.weight_dtype = weight_dtype;
  q_params.alg = alg;
  q_params.group_size = group_size;
  q_params.scale_dtype = scale_dtype;
  q_params.compute_dtype = compute_dtype;
  q_params.use_ggml = use_ggml;
  q_params.nthread = threads;

  ne_ftype ftype = quant_params_to_ftype(q_params);
  printf("ne_ftype: %d\n", ftype);

  auto quant_layer = get_model_quant_layer(q_params.model_name);
  if (model_quantize(q_params, quant_layer)) {
    fprintf(stderr, "%s: failed to quantize model from '%s'\n", __func__, q_params.model_file.c_str());
    return 1;
  }
  return 0;
}

size_t Model::jblas_qpack(const int8_t* src_w, const float* src_scales, const int8_t* src_zps, void* dstpr,
                          const quant_params_internal params, int nthread, int n, int k) {
  using CompType = jblas::prologue::weight_comp::gemm_kblcok::PrologueBIDs;
  using namespace ne_jblas;
  auto cd = jblas::utils::parallel::CpuDevice::getInstance();
  auto dstbptr = (int8_t*)dstpr;
  cd->setThreads(nthread);
  // int8: using Kernel = WeiS8Fp32<GcCompInt8KBlock, JblasAVX512F>;
  using Kernel = WeiS4ClipFp32<GcCompInt8KBlock, JblasAVX512F>;
  static Kernel kernel;
  auto packedw = kernel.createStorage(n, k, params.group_size);

  // jblas::utils::aligned_vector<int8_t> buffer(packedw.mSize);
  packedw.assign(dstbptr);

  jblas::utils::aligned_vector<int8_t> tmpq(n * k);
  std::copy(src_w, src_w + n * k, tmpq.data());

  int nk_scale = jblas::utils::updiv(k, packedw.mBlockSize);
  auto ssize = (size_t)n * nk_scale;
  jblas::utils::avector<float> Tscales(ssize);
  std::copy(src_scales, src_scales + ssize, Tscales.data());

  jblas::utils::avector<int8_t> Tzps(packedw.mIsAsym ? ssize : 0);

  kernel.packQWeight(n, k, tmpq.data(), n, Tscales.data(), Tzps.data(), &packedw);

  // kernel.unpackWeight(n, k, &packedw, dstbptr, n);
  return packedw.mSize;
}

size_t Model::jblas_quantize(const float* src_w, void* dstpr, const quant_params_internal params, int nthread, int n,
                             int k) {
  using CompType = jblas::prologue::weight_comp::gemm_kblcok::PrologueBIDs;
  using namespace ne_jblas;
  auto cd = jblas::utils::parallel::CpuDevice::getInstance();
  auto dstbptr = (int8_t*)dstpr;
  cd->setThreads(nthread);
  // using Kernel = WeiS8Fp32<GcCompInt8KBlock, JblasAVX512F>;
  using Kernel = WeiS4ClipFp32<GcCompInt8KBlock, JblasAVX512F>;
  static Kernel kernel;
  auto packedw = kernel.createStorage(n, k, params.group_size);

  // jblas::utils::aligned_vector<int8_t> buffer(packedw.mSize);
  packedw.assign(dstbptr);

  kernel.packTransposeWeight(n, k, src_w, k, &packedw);
  // kernel.unpackTransposeWeight(n, k, &packedw, dstbptr, n);
  return packedw.mSize;
}

#if MODEL_NAME_ID == 1

PYBIND11_MODULE(gptj_cpp, m)
#elif MODEL_NAME_ID == 2

PYBIND11_MODULE(falcon_cpp, m)

#elif MODEL_NAME_ID == 3

PYBIND11_MODULE(gptneox_cpp, m)

#elif MODEL_NAME_ID == 4

PYBIND11_MODULE(dolly_cpp, m)

#elif MODEL_NAME_ID == 5

PYBIND11_MODULE(llama_cpp, m)

#elif MODEL_NAME_ID == 6

PYBIND11_MODULE(mpt_cpp, m)

#elif MODEL_NAME_ID == 7

PYBIND11_MODULE(starcoder_cpp, m)

#elif MODEL_NAME_ID == 8

PYBIND11_MODULE(opt_cpp, m)

#elif MODEL_NAME_ID == 9

PYBIND11_MODULE(bloom_cpp, m)

#elif MODEL_NAME_ID == 10

PYBIND11_MODULE(chatglm2_cpp, m)

#elif MODEL_NAME_ID == 11

PYBIND11_MODULE(chatglm_cpp, m)

#elif MODEL_NAME_ID == 12

PYBIND11_MODULE(baichuan_cpp, m)

#elif MODEL_NAME_ID == 13

PYBIND11_MODULE(polyglot_cpp, m)

#elif MODEL_NAME_ID == 14

PYBIND11_MODULE(mistral_cpp, m)

#elif MODEL_NAME_ID == 15

PYBIND11_MODULE(qwen_cpp, m)

#endif
{
  m.doc() = "cpp model python binding";
  py::class_<Model>(m, "Model", py::module_local())
      .def(py::init())
      .def("init_model", &Model::init_model, "initial model with model path and parameters", py::arg("model_path"),
           py::arg("max_new_tokens") = -1, py::arg("n_batch") = 512, py::arg("ctx_size") = 512, py::arg("seed") = -1,
           py::arg("threads") = 8, py::arg("repetition_penalty") = 1.1f, py::arg("num_beams") = 1,
           py::arg("do_sample") = false, py::arg("top_k") = 40, py::arg("top_p") = 0.95, py::arg("temperature") = 0.8,
           py::arg("min_new_tokens") = 0, py::arg("length_penalty") = 1.0, py::arg("early_stopping") = false,
           py::arg("n_keep") = 0, py::arg("n_discard") = -1, py::arg("shift_roped_k") = false,
           py::arg("batch_size") = 1, py::arg("pad_token") = -1, py::arg("memory_dtype") = "auto")
      .def("generate", &Model::generate, "Generate token with input ids", py::arg("input_ids"))
      .def("evaluate", &Model::evaluate, "Evaluate token with input ids and output logits",
           py::arg("input_ids") = std::vector<std::vector<model_token>>{})
      .def("generate_tokens", &Model::generate_tokens, "Generate tokens with input ids", py::arg("input_ids"))
      .def_static("quant_model", &Model::quant_model, "Quantize model", py::arg("model_path"), py::arg("out_path"),
                  py::arg("weight_dtype") = "int4", py::arg("alg") = "sym", py::arg("group_size") = 32,
                  py::arg("scale_dtype") = "fp32", py::arg("compute_dtype") = "int8", py::arg("use_ggml") = false,
                  py::arg("threads") = 8)
      .def("is_token_end", &Model::is_token_end)
      .def("reset_token_end", &Model::reset_token_end)
      .def_static("np_jblas_qpack", &Model::np_jblas_qpack)
      .def_static("np_jblas_quantize", &Model::np_jblas_quantize)
      .def("reinit", &Model::reinit);
}
