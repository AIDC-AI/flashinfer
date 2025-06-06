/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: NVIDIA TensorRT Source Code License Agreement
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include <cutlass_extensions/epilogue_helpers.h>
#include <tensorrt_llm/common/cudaUtils.h>

#include <cutlass_extensions/gemm/kernel/fused_moe_kernel.cuh>

#include "cute/tensor.hpp"
#include "cutlass/array.h"
#include "cutlass/cutlass.h"
#include "cutlass/gemm/device/gemm_grouped.h"
#include "cutlass/gemm/kernel/default_gemm_grouped.h"
#include "cutlass/numeric_conversion.h"

namespace tensorrt_llm::kernels::cutlass_kernels {
template <typename ElementType_, typename CutlassWeightType_, int MaxTileM_, int TileN_, int TileK_,
          int Stages_, typename EpilogueTag>
void sm80_generic_fused_moe_gemm_kernelLauncher(ElementType_ const* A, CutlassWeightType_ const* B,
                                                ElementType_ const* biases, bool bias_is_broadcast,
                                                ElementType_* C,
                                                int64_t const* total_tokens_including_expert,
                                                int64_t num_rows, int64_t gemm_n, int64_t gemm_k,
                                                int num_experts, int multi_processor_count,
                                                cudaStream_t stream, int* kernel_occupancy) {
  constexpr auto activation_type = fused_moe::EpilogueRouting<EpilogueTag>(true);
  using GemmType =
      fused_moe::Fused_Moe_Kernel_sm80<ElementType_, CutlassWeightType_, ElementType_, MaxTileM_,
                                       TileN_, TileK_, Stages_, activation_type>;

  // make sure GPU has enough resources..
  if (kernel_occupancy != nullptr) {
    constexpr int smem_size = GemmType::kSmemSize;

    if (smem_size > (48 << 10)) {
      cudaFuncAttributes attr{};
      int device = 0;
      int max_smem_per_block = 0;
      tensorrt_llm::common::check_cuda_error(cudaGetDevice(&device));
      tensorrt_llm::common::check_cuda_error(cudaDeviceGetAttribute(
          &max_smem_per_block, cudaDevAttrMaxSharedMemoryPerBlockOptin, device));
      tensorrt_llm::common::check_cuda_error(
          cudaFuncGetAttributes(&attr, fused_moe::run_global<GemmType>));
      if (smem_size + attr.sharedSizeBytes >= static_cast<size_t>(max_smem_per_block)) {
        // This should mean that
        // cudaFuncSetAttribute(cutlass::Kernel<GemmKernel>,
        // cudaFuncAttributeMaxDynamicSharedMemorySize, smem_size) wouldn't work. In that case, we
        // return an occupancy of 0. This will cause the heuristic to ignore this configuration.
        *kernel_occupancy = 0;
        return;
      }
    }

    int max_active_blocks = -1;
    tensorrt_llm::common::check_cuda_error(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &max_active_blocks, fused_moe::run_global<GemmType>, GemmType::kThreadCount, smem_size));
    *kernel_occupancy = max_active_blocks;
    return;
  }
  int occupancy = std::min(2, fused_moe::fused_gemm_maximum_active_blocks<GemmType>());
  int const threadblock_count = multi_processor_count * occupancy;
  TLLM_CHECK_WITH_INFO(occupancy > 0,
                       "GPU lacks the shared memory resources to run fused_moe kernel");
  using Arguments = typename GemmType::Arguments;
  Arguments args{{const_cast<ElementType_*>(A), const_cast<CutlassWeightType_*>(B),
                  const_cast<ElementType_*>(biases), reinterpret_cast<ElementType_*>(C),
                  total_tokens_including_expert, static_cast<int>(gemm_n), static_cast<int>(gemm_k),
                  num_experts, bias_is_broadcast},
                 num_experts,
                 threadblock_count};
  auto params = GemmType::to_underlying_arguments(args);
  if (GemmType::kSmemSize >= (48 << 10)) {
    cudaError_t result =
        cudaFuncSetAttribute(fused_moe::run_global<GemmType>,
                             cudaFuncAttributeMaxDynamicSharedMemorySize, GemmType::kSmemSize);
    TLLM_CHECK_WITH_INFO(result == cudaSuccess, "Fail to set the max smem size to " +
                                                    std::to_string(GemmType::kSmemSize) +
                                                    " for fused moe kernel");
  }
  dim3 grid(params.threadblock_count, 1, 1);
  dim3 block(GemmType::kThreadCount);
  fused_moe::run_global<GemmType><<<grid, block, GemmType::kSmemSize, stream>>>(params);
  auto result = cudaGetLastError();
  TLLM_CHECK_WITH_INFO(result == cudaSuccess, "Fail to execute fused moe kernel, cuda error %d\n",
                       (int)(result));
}
}  // namespace tensorrt_llm::kernels::cutlass_kernels
