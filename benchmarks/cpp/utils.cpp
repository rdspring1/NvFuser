// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <benchmarks/cpp/utils.h>
#include <c10/cuda/CUDACachingAllocator.h>
#include <cuda_utils.h>

#include <fusion_profiler.h>
#include <scheduler/all_schedulers.h>
#include <tests/cpp/utils.h>

#include <sstream>

using namespace nvfuser;

std::string toString(const ReductionParams* rparams) {
  std::stringstream ss;
  ss << (rparams->fastest_dim ? "Red On Fastest Dim // "
                              : "Red On Slow Dim // ")
     << (rparams->persistent_kernel ? "Persistent Kernel // " : "")
     << (rparams->project_persistent_buffers ? "Project Persistent Buffers // "
                                             : "");

  if (rparams->schedule_3D) {
    ss << "3D Schedule // "
       << "Outer Reduction: "
       << (rparams->cross_block_outer_reduction ? "cross block / " : "")
       << (rparams->cross_grid_outer_reduction ? "cross grid / " : "")
       << (rparams->split_grid_dim_outer_reduction ? "split grid dim / " : "");
    if (rparams->batches_per_block_outer_reduction > 1 ||
        rparams->persistent_kernel) {
      ss << "persistent batch - " << rparams->batches_per_block_outer_reduction
         << " / ";
    }
  }

  ss << " // Iteration Domain: "
     << (rparams->multiple_reds_per_blk ? "multiple reductions per block / "
                                        : "")
     << ((rparams->split_grid_dim_iter_dom_inner ||
          rparams->split_grid_dim_iter_dom_outer)
             ? "split grid dimension / "
             : "")
     << (rparams->vectorize_iter_dom ? "vectorize / " : "")
     << (rparams->unroll_factor_iter_dom > 1 && !rparams->vectorize_iter_dom
             ? "unroll / "
             : "");
  if (rparams->unroll_factor_iter_dom > 1 || rparams->vectorize_iter_dom) {
    ss << "factor " << rparams->unroll_factor_iter_dom;
  }

  ss << " // Inner Reduction Domain: "
     << (rparams->cross_block_inner_reduction ? "cross block reduction / " : "")
     << (rparams->pad_inner_reduction_to_warp ? "pad to warp / " : "")
     << (rparams->cross_grid_inner_reduction ? "cross grid reduction / " : "");

  if (rparams->batches_per_block_inner_reduction > 1 ||
      rparams->persistent_kernel) {
    ss << "persistent batch - " << rparams->batches_per_block_inner_reduction
       << " / ";
  }

  ss << (rparams->cross_grid_inner_reduction &&
                 rparams->split_grid_dim_inner_reduction
             ? "split grid dimension / "
             : "")
     << (rparams->vectorize_inner_reduction ? "vectorize / " : "")
     << (rparams->unroll_factor_inner_reduction > 1 &&
                 !rparams->vectorize_inner_reduction
             ? "unroll / "
             : "");
  if (rparams->unroll_factor_inner_reduction > 1 ||
      rparams->vectorize_inner_reduction) {
    ss << "factor " << rparams->unroll_factor_inner_reduction;
  }
  return ss.str();
}

std::string toString(const PointwiseParams* pparams) {
  std::stringstream ss;
  if (pparams->break_point) {
    ss << "2D Schedule at " << pparams->break_point << "/";
    if (pparams->split_block) {
      ss << " Split block into y-dim/";
    }
    if (pparams->split_grid_y_dim) {
      ss << " Split y grid dim/";
    }
  } else {
    ss << "1D"
       << "/";
  }
  if (pparams->vectorization_factor > 1) {
    ss << "Vectorize, Factor: " << pparams->vectorization_factor << "\n";
  }
  if (pparams->unroll_factor_outer > 1) {
    ss << "Outer Unroll, Factor: " << pparams->unroll_factor_outer << "\n";
  }
  if (pparams->unroll_factor_inner > 1) {
    ss << "Inner Unroll, Factor: " << pparams->unroll_factor_inner << "\n";
  }
  return ss.str();
}

std::string toString(const TransposeParams* tparams) {
  std::stringstream ss;
  ss << "Tile size: (" << tparams->tile_size1 << "," << tparams->tile_size2
     << ")/";
  ss << "Vectorize size: (" << tparams->vectorize_factor1 << ","
     << tparams->vectorize_factor2 << ")";
  return ss.str();
}

std::string toString(const std::unique_ptr<HeuristicParams>& params) {
  auto rparams = dynamic_cast<const ReductionParams*>(params.get());
  if (rparams) {
    return toString(rparams);
  }
  auto pparams = dynamic_cast<const PointwiseParams*>(params.get());
  if (pparams) {
    return toString(pparams);
  }
  auto tparams = dynamic_cast<const TransposeParams*>(params.get());
  if (tparams) {
    return toString(tparams);
  }
  NVF_THROW(
      "Unknown heuristic parameter type. Did you just added a new heuristic parameter type but forget to update here?");
}

std::string toString(LaunchParams lparams) {
  std::stringstream ss;
  lparams.toString();
  ss << "/Launch_Parameters["
     << "block(" << lparams.bdimz() << "/" << lparams.bdimy() << "/"
     << lparams.bdimx() << ")/grid(" << lparams.gdimz() << "/"
     << lparams.gdimy() << "/" << lparams.gdimx() << ")/" << lparams.smem()
     << "]";
  return ss.str();
}

namespace {

int64_t getSizeOfInputs(const std::vector<c10::IValue>& inputs) {
  int64_t bytes = 0;
  for (const auto& inp : inputs) {
    if (!inp.isTensor()) {
      continue;
    }
    const auto& inp_tensor = inp.toTensor();
    bytes += inp_tensor.numel() *
        (int64_t)dataTypeSize(aten_to_data_type(inp_tensor.scalar_type()));
  }
  return bytes;
}

int64_t getSizeOfOutputs(const std::vector<at::Tensor>& outputs) {
  int64_t bytes = 0;
  for (const auto& tensor : outputs) {
    bytes += tensor.numel() *
        (int64_t)dataTypeSize(aten_to_data_type(tensor.scalar_type()));
  }
  return bytes;
}
} // namespace

int64_t runBenchmarkIterations(
    benchmark::State& benchmark_state,
    FusionExecutorCache* executor_cache,
    std::vector<c10::IValue>& aten_inputs) {
  c10::cuda::CUDACachingAllocator::emptyCache();
  executor_cache->profile(true);

  int64_t io_bytes = getSizeOfInputs(aten_inputs);

  // Segment and compile the fusion
  {
    auto cg_outputs = executor_cache->runFusionWithInputs(aten_inputs);
    io_bytes += getSizeOfOutputs(cg_outputs);
  }

  bool segmented =
      executor_cache->getMostRecentKernelRuntime()->isSegmented() &&
      executor_cache->getMostRecentKernelRuntime()
              ->fusionSegments()
              ->groups()
              .size() > 1;

  // Only set if not segmented. In the case of segmented fusions,
  // this could be confusing as the log would refect only the last
  // segment. Revisit if necessary.
  if (!segmented) {
    const auto& compile_log = executor_cache->getMostRecentExecutorInfo();
    auto params = toString(compile_log.params);
    auto lparams = toString(
        compile_log.fusion_executor->as<KernelExecutor>()->lastLaunchParams());
    benchmark_state.SetLabel(params + lparams);
  }

  executor_cache->profile(false);

  // Sync everything up before we start
  NVFUSER_CUDA_RT_SAFE_CALL(cudaDeviceSynchronize());
  ProfilerOptionsGuard::getCurOptions().set(ProfilerOption::Enable);

  for (auto _ : benchmark_state) {
    clearL2Cache();
    auto cg_outputs = executor_cache->runFusionWithInputs(aten_inputs);
    benchmark_state.SetIterationTime(
        FusionProfiler::profile().kernel_time_ms / 1000.0);
  }

  ProfilerOptionsGuard::getCurOptions().unset(ProfilerOption::Enable);
  // Sync everything up before we're finished, don't want to run ahead on the
  // cpu while benchmarking.
  NVFUSER_CUDA_RT_SAFE_CALL(cudaDeviceSynchronize());

  return io_bytes;
}

int64_t runBenchmarkIterations(
    benchmark::State& benchmark_state,
    KernelExecutor* ke,
    std::vector<c10::IValue>& aten_inputs,
    const LaunchParams& launch_constraints,
    CompileParams compile_params) {
  int64_t io_bytes = getSizeOfInputs(aten_inputs);
  {
    // Warm-up run
    auto cg_outputs = ke->run(aten_inputs, launch_constraints, compile_params);
    io_bytes += getSizeOfOutputs(cg_outputs);
  }

  auto lparams = toString(ke->lastLaunchParams());
  benchmark_state.SetLabel(lparams);

  // Sync everything up before we start
  NVFUSER_CUDA_RT_SAFE_CALL(cudaDeviceSynchronize());
  ProfilerOptionsGuard::getCurOptions().set(ProfilerOption::Enable);

  for (auto _ : benchmark_state) {
    clearL2Cache();
    FusionProfiler::start();
    FusionProfiler::createSegments(1);
    auto cg_outputs = ke->run(aten_inputs, launch_constraints, compile_params);
    FusionProfiler::stop();
    benchmark_state.SetIterationTime(
        FusionProfiler::profile().kernel_time_ms / 1000.0);
  }

  ProfilerOptionsGuard::getCurOptions().unset(ProfilerOption::Enable);
  // Sync everything up before we're finished, don't want to run ahead on the
  // cpu while benchmarking.
  NVFUSER_CUDA_RT_SAFE_CALL(cudaDeviceSynchronize());

  return io_bytes;
}

namespace executorCache {
thread_local ExecutorMap executor_map_;
ExecutorMap& getGlobalMap() {
  return executor_map_;
}
} // namespace executorCache

// Utility functions for adding cases to benchmarks.
// Range increment is not available from the public API.
void addCases16Wave128To32K(benchmark::internal::Benchmark* b) {
  const auto properties = at::cuda::getCurrentDeviceProperties();
  int batch_size = 16 * properties->multiProcessorCount;
  for (auto hidden_size = 128; hidden_size <= 32768; hidden_size += 128) {
    b->Args({batch_size, hidden_size});
  }
}

void addCasesOneWave128To32K(benchmark::internal::Benchmark* b) {
  const auto properties = at::cuda::getCurrentDeviceProperties();
  int batch_size = properties->multiProcessorCount;
  for (auto hidden_size = 128; hidden_size <= 32768; hidden_size += 128) {
    b->Args({batch_size, hidden_size});
  }
}
