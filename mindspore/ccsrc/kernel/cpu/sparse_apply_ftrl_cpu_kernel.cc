/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "kernel/cpu/sparse_apply_ftrl_cpu_kernel.h"
#include "kernel/common_utils.h"
#include "device/cpu/cpu_device_address.h"

namespace mindspore {
namespace kernel {
namespace {
constexpr size_t kSparseApplyFtrlInputSize = 5;
}  // namespace

void SparseApplyFtrlCPUKernel::InitInputOutputSize(const CNodePtr &kernel_node) {
  CPUKernel::InitInputOutputSize(kernel_node);
  MS_EXCEPTION_IF_NULL(kernel_node);
  workspace_size_list_.emplace_back(indices_size_ * var_outer_dim_size_ * sizeof(float));
  workspace_size_list_.emplace_back(indices_size_ * sizeof(int));
}

void SparseApplyFtrlCPUKernel::InitKernel(const CNodePtr &kernel_node) {
  MS_EXCEPTION_IF_NULL(kernel_node);
  std::vector<size_t> var_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 0);
  std::vector<size_t> accum_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 1);
  std::vector<size_t> linear_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 2);
  std::vector<size_t> grad_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 3);
  std::vector<size_t> indices_shape = AnfAlgo::GetPrevNodeOutputInferShape(kernel_node, 4);
  if (!IsSameShape(var_shape, accum_shape)) {
    MS_LOG(EXCEPTION) << "var and accum should have the same shape";
  }
  if (!IsSameShape(var_shape, linear_shape)) {
    MS_LOG(EXCEPTION) << "var and linear should have the same shape";
  }
  if (var_shape.empty()) {
    MS_LOG(EXCEPTION) << "var must be at least 1D";
  }
  var_first_dim_size_ = var_shape[0];
  for (size_t i = 1; i < var_shape.size(); ++i) {
    if (var_shape[i] != grad_shape[i]) {
      MS_LOG(EXCEPTION) << "The shape of var and grad must equal in dimension " << i;
    }
    var_outer_dim_size_ *= var_shape[i];
  }
  if (indices_shape.size() != 1) {
    MS_LOG(EXCEPTION) << "indices must be a 1D vector";
  }
  indices_size_ = indices_shape[0];
  if (grad_shape[0] != indices_size_) {
    MS_LOG(EXCEPTION) << "The first dimension of grad shape must be equal to indices";
  }
  lr_ = AnfAlgo::GetNodeAttr<float>(kernel_node, "lr");
  if (lr_ <= 0) {
    MS_LOG(EXCEPTION) << "lr should be a positive scalar";
  }
  l1_ = AnfAlgo::GetNodeAttr<float>(kernel_node, "l1");
  if (l1_ < 0) {
    MS_LOG(EXCEPTION) << "l1 should be a non-negative scalar";
  }
  l2_ = AnfAlgo::GetNodeAttr<float>(kernel_node, "l2");
  if (l2_ < 0) {
    MS_LOG(EXCEPTION) << "l2 should be a non-negative scalar";
  }
  lr_power_ = AnfAlgo::GetNodeAttr<float>(kernel_node, "lr_power");
  if (lr_power_ > 0) {
    MS_LOG(EXCEPTION) << "lr_power should be a non-positive scalar";
  }
}

bool SparseApplyFtrlCPUKernel::Launch(const std::vector<kernel::AddressPtr> &inputs,
                                      const std::vector<kernel::AddressPtr> &workspace,
                                      const std::vector<kernel::AddressPtr> & /*outputs*/) {
  if (inputs.size() < kSparseApplyFtrlInputSize) {
    MS_LOG(EXCEPTION) << "error input output size!";
  }

  auto var = reinterpret_cast<float *>(inputs[0]->addr);
  auto accum = reinterpret_cast<float *>(inputs[1]->addr);
  auto linear = reinterpret_cast<float *>(inputs[2]->addr);
  auto grad = reinterpret_cast<float *>(inputs[3]->addr);
  auto indices = reinterpret_cast<int *>(inputs[4]->addr);
  auto new_grad = reinterpret_cast<float *>(workspace[0]->addr);
  auto new_indices = reinterpret_cast<int *>(workspace[1]->addr);
  SparseGradient unique_sparse_grad({new_grad, new_indices, indices_size_});
  ReduceSparseGradient(SparseGradient({grad, indices, indices_size_}), &unique_sparse_grad, var_first_dim_size_,
                       var_outer_dim_size_);

  for (size_t i = 0; i < unique_sparse_grad.indices_size_; ++i) {
    int index = unique_sparse_grad.indices_[i];
    if (index < 0 || IntToSize(index) >= var_first_dim_size_) {
      MS_LOG(EXCEPTION) << "Index " << index << " in indices is out of range after unique process";
    }
    size_t start_index = var_outer_dim_size_ * index;
    size_t end_index = start_index + var_outer_dim_size_;
    for (size_t j = start_index, k = var_outer_dim_size_ * i; j < end_index; ++j, ++k) {
      auto summed_grad = unique_sparse_grad.value_[k];
      auto accum_new = accum[j] + summed_grad * summed_grad;
      if (lr_power_ == -0.5) {
        linear[j] += summed_grad - (std::sqrt(accum_new) - std::sqrt(accum[j])) / lr_ * var[j];
      } else {
        linear[j] += summed_grad - (std::pow(accum_new, -lr_power_) - std::pow(accum[j], -lr_power_)) / lr_ * var[j];
      }
      auto x = Sign(linear[j]) * l1_ - linear[j];
      float y;
      if (lr_power_ == -0.5) {
        y = std::sqrt(accum_new) / lr_ + 2 * l2_;
      } else {
        y = std::pow(accum_new, -lr_power_) / lr_ + 2 * l2_;
      }
      auto pre_shrink = x / y;
      var[j] = std::fabs(linear[j]) > l1_ ? pre_shrink : 0;
      accum[j] = accum_new;
    }
  }
  return true;
}
}  // namespace kernel
}  // namespace mindspore
