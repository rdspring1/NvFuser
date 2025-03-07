// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <csrc/exceptions.h>
#include <device_lower/lower2device.h>
#include <fusion.h>
#include <ir/all_nodes.h>
#include <ir/builder.h>
#include <ir/utils.h>
#include <ops/all_ops.h>
#include <runtime/executor.h>
#include <scheduler/all_schedulers.h>

#include <benchmark/benchmark.h>

#include <cuda_runtime.h>

#include <benchmarks/cpp/utils.h>
#include <tests/cpp/utils.h>

using namespace nvfuser;

static auto getLayerBackwardNormRuntime(
    std::unique_ptr<Fusion> fusion_ptr,
    std::unique_ptr<FusionExecutorCache>& executor_cache,
    std::vector<c10::IValue>& aten_inputs,
    std::vector<int64_t>& shape,
    std::vector<int64_t>& norm_shape) {
  Fusion& fusion = *fusion_ptr.get();

  const size_t kM = shape.size();
  const size_t kN = norm_shape.size();
  const size_t kOuterNumDims = kM - kN;

  std::vector<int64_t> outer_shape;
  for (size_t idx = 0; idx < kOuterNumDims; ++idx) {
    outer_shape.push_back(shape[idx]);
  }
  for (size_t idx = kOuterNumDims; idx < kM; ++idx) {
    outer_shape.push_back(1);
  }

  auto grad_out = makeSymbolicTensor(shape.size());
  auto input = makeSymbolicTensor(shape.size());
  auto mean = makeConcreteTensor(outer_shape);
  auto rstd = makeConcreteTensor(outer_shape);
  auto weight = makeSymbolicTensor(norm_shape.size());
  auto bias = makeSymbolicTensor(norm_shape.size());
  fusion.addInput(grad_out);
  fusion.addInput(input);
  fusion.addInput(mean);
  fusion.addInput(rstd);
  fusion.addInput(weight);
  fusion.addInput(bias);

  auto grads = layer_norm_backward(
      grad_out,
      input,
      norm_shape,
      mean,
      rstd,
      weight,
      bias,
      {true, true, true});

  fusion.addOutput(grads.grad_input);
  fusion.addOutput(grads.grad_weight);
  fusion.addOutput(grads.grad_bias);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor aten_grad_out = at::randn(shape, options);
  at::Tensor aten_input = at::randn(shape, options);
  at::Tensor aten_weight = at::randn(norm_shape, options);
  at::Tensor aten_bias = at::randn(norm_shape, options);
  auto at_weight = c10::optional<at::Tensor>(aten_weight);
  auto at_bias = c10::optional<at::Tensor>(aten_bias);

  const float kEps = 1e-5;
  auto aten_results =
      at::native_layer_norm(aten_input, norm_shape, at_weight, at_bias, kEps);
  auto aten_output = std::get<0>(aten_results);
  auto aten_mean = std::get<1>(aten_results);
  auto aten_rstd = std::get<2>(aten_results);

  executor_cache = std::make_unique<FusionExecutorCache>(std::move(fusion_ptr));
  aten_inputs = {
      aten_grad_out, aten_input, aten_mean, aten_rstd, aten_weight, aten_bias};
  auto cg_outputs = executor_cache->runFusionWithInputs(aten_inputs);

  return executor_cache->getMostRecentKernelRuntime();
}

void LayerNormBackward_ShapeInference_Base(
    benchmark::State& benchmark_state,
    bool disable_launch_parameter_cache) {
  std::unique_ptr<Fusion> fusion_ptr = std::make_unique<Fusion>();
  FusionGuard fg(fusion_ptr.get());

  // PreAllocate
  std::unique_ptr<FusionExecutorCache> executor_cache;
  std::vector<c10::IValue> aten_inputs;

  std::vector<int64_t> shape{20, 100, 35, 67};
  std::vector<int64_t> norm_shape{67};

  auto runtime = getLayerBackwardNormRuntime(
      std::move(fusion_ptr), executor_cache, aten_inputs, shape, norm_shape);

  KernelArgumentHolder args =
      KernelArgumentHolder::createKernelArgumentHolder(aten_inputs);

  NVF_ERROR(runtime->getMaybeHeuristicsFor(args).has_value());

  executor_cache->profile(true);
  executor_cache->disableKernelLaunch();
  executor_cache->runFusionWithInputs(aten_inputs);
  if (disable_launch_parameter_cache) {
    executor_cache->disableLaunchParamCache();
  }

  for (auto _ : benchmark_state) {
    // Setup (not included in the measurement)
    executor_cache->runFusionWithInputs(aten_inputs);
  }
}

static void NvFuserScheduler_LayerNormBackward_ShapeInference(
    benchmark::State& benchmark_state) {
  LayerNormBackward_ShapeInference_Base(benchmark_state, true);
}

static void NvFuserScheduler_LayerNormBackward_NoShapeInferenceCachedBaseline(
    benchmark::State& benchmark_state) {
  LayerNormBackward_ShapeInference_Base(benchmark_state, false);
}

static auto getLayerForwardNormRuntime(
    std::unique_ptr<Fusion> fusion_ptr,
    std::unique_ptr<FusionExecutorCache>& executor_cache,
    std::vector<c10::IValue>& aten_inputs,
    std::vector<int64_t>& shape,
    std::vector<int64_t>& norm_shape) {
  Fusion& fusion = *fusion_ptr.get();

  const float kEps = 1e-5;
  Val* eps_ptr = IrBuilder::create<Val>(kEps);

  auto input = makeSymbolicTensor(shape.size());
  fusion.addInput(input);

  auto result = layer_norm(input, norm_shape, nullptr, nullptr, eps_ptr);

  fusion.addOutput(result.output);
  fusion.addOutput(result.mean);
  fusion.addOutput(result.invstd);

  auto options = at::TensorOptions().dtype(at::kFloat).device(at::kCUDA, 0);
  at::Tensor aten_input = at::randn(shape, options);

  executor_cache = std::make_unique<FusionExecutorCache>(std::move(fusion_ptr));
  aten_inputs = {aten_input};
  auto cg_outputs = executor_cache->runFusionWithInputs(aten_inputs);

  return executor_cache->getMostRecentKernelRuntime();
}

void LayerNormForward_ShapeInferenceBase(
    benchmark::State& benchmark_state,
    bool disable_launch_param_cache) {
  std::unique_ptr<Fusion> fusion_ptr = std::make_unique<Fusion>();
  FusionGuard fg(fusion_ptr.get());

  // PreAllocate
  std::unique_ptr<FusionExecutorCache> executor_cache;
  std::vector<c10::IValue> aten_inputs;

  std::vector<int64_t> shape{20, 100, 35, 67};
  std::vector<int64_t> norm_shape{67};

  auto runtime = getLayerForwardNormRuntime(
      std::move(fusion_ptr), executor_cache, aten_inputs, shape, norm_shape);

  KernelArgumentHolder args =
      KernelArgumentHolder::createKernelArgumentHolder(aten_inputs);

  NVF_ERROR(runtime->getMaybeHeuristicsFor(args).has_value());

  executor_cache->profile(true);
  executor_cache->disableKernelLaunch();
  executor_cache->runFusionWithInputs(aten_inputs);

  if (disable_launch_param_cache) {
    executor_cache->disableLaunchParamCache();
  }

  for (auto _ : benchmark_state) {
    // Setup (not included in the measurement)
    executor_cache->runFusionWithInputs(aten_inputs);
  }
}

static void NvFuserScheduler_LayerNormForward_NoShapeInferenceCachedBaseline(
    benchmark::State& benchmark_state) {
  LayerNormForward_ShapeInferenceBase(benchmark_state, false);
}

static void NvFuserScheduler_LayerNormForward_ShapeInference(
    benchmark::State& benchmark_state) {
  LayerNormForward_ShapeInferenceBase(benchmark_state, true);
}

BENCHMARK(NvFuserScheduler_LayerNormBackward_ShapeInference)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(NvFuserScheduler_LayerNormForward_ShapeInference)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(NvFuserScheduler_LayerNormBackward_NoShapeInferenceCachedBaseline)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(NvFuserScheduler_LayerNormForward_NoShapeInferenceCachedBaseline)
    ->Unit(benchmark::kMicrosecond);
