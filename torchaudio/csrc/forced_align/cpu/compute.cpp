#include <torch/script.h>
#include <torch/torch.h>

using namespace std;

namespace torchaudio {
namespace alignment {
namespace cpu {
// Inspired from
// https://github.com/flashlight/sequence/blob/main/flashlight/lib/sequence/criterion/cpu/ConnectionistTemporalClassificationCriterion.cpp
template <typename scalar_t, at::ScalarType target_scalar_type>
void forced_align_impl(
    const torch::Tensor& logProbs,
    const torch::Tensor& targets,
    const int64_t blank,
    torch::Tensor& paths) {
  const scalar_t kNegInfinity = -std::numeric_limits<scalar_t>::infinity();
  using target_t = typename std::
      conditional<target_scalar_type == torch::kInt, int, int64_t>::type;
  const auto T = logProbs.size(0);
  const auto L = targets.size(0);
  const auto S = 2 * L + 1;
  torch::Tensor alphas = torch::empty(
                             {2, S},
                             torch::TensorOptions()
                                 .device(logProbs.device())
                                 .dtype(logProbs.dtype()))
                             .fill_(kNegInfinity);
  torch::Tensor backPtr = torch::empty({T, S}, torch::kInt8).fill_(-1);
  auto logProbs_a = logProbs.accessor<scalar_t, 2>();
  auto targets_a = targets.accessor<target_t, 1>();
  auto paths_a = paths.accessor<target_t, 1>();
  auto alphas_a = alphas.accessor<scalar_t, 2>();
  auto backPtr_a = backPtr.accessor<int8_t, 2>();
  auto R = 0;
  for (auto i = 1; i < L; i++) {
    if (targets_a[i] == targets_a[i - 1]) {
      ++R;
    }
  }
  TORCH_CHECK(
      T >= L + R,
      "targets length is too long for CTC. Found targets length: ",
      T,
      ", log_probs length: ",
      L,
      ", and number of repeats: ",
      R);
  auto start = T - (L + R) > 0 ? 0 : 1;
  auto end = (S == 1) ? 1 : 2;
  for (auto i = start; i < end; i++) {
    auto labelIdx = (i % 2 == 0) ? blank : targets_a[i / 2];
    alphas_a[0][i] = logProbs_a[0][labelIdx];
  }
  for (auto t = 1; t < T; t++) {
    if (T - t <= L + R) {
      if ((start % 2 == 1) &&
          targets_a[start / 2] != targets_a[start / 2 + 1]) {
        start = start + 1;
      }
      start = start + 1;
    }
    if (t <= L + R) {
      if (end % 2 == 0 && end < 2 * L &&
          targets_a[end / 2 - 1] != targets_a[end / 2]) {
        end = end + 1;
      }
      end = end + 1;
    }
    auto startloop = start;
    auto curIdxOffset = t % 2;
    auto prevIdxOffset = (t - 1) % 2;
    for (auto j = 0; j < S; ++j) {
      alphas_a[curIdxOffset][j] = -std::numeric_limits<scalar_t>::infinity();
    }
    if (start == 0) {
      alphas_a[curIdxOffset][0] =
          alphas_a[prevIdxOffset][0] + logProbs_a[t][blank];
      backPtr_a[t][0] = 0;
      startloop += 1;
    }

    for (auto i = startloop; i < end; i++) {
      auto x0 = alphas_a[prevIdxOffset][i];
      auto x1 = alphas_a[prevIdxOffset][i - 1];
      auto x2 = -std::numeric_limits<scalar_t>::infinity();

      auto labelIdx = (i % 2 == 0) ? blank : targets_a[i / 2];

      // In CTC, the optimal path may optionally chose to skip a blank label.
      // x2 represents skipping a letter, and can only happen if we're not
      // currently on a blank_label, and we're not on a repeat letter
      // (i != 1) just ensures we don't access targets[i - 2] if its i < 2
      if (i % 2 != 0 && i != 1 && targets_a[i / 2] != targets_a[i / 2 - 1]) {
        x2 = alphas_a[prevIdxOffset][i - 2];
      }
      scalar_t result = 0.0;
      if (x2 > x1 && x2 > x0) {
        result = x2;
        backPtr_a[t][i] = 2;
      } else if (x1 > x0 && x1 > x2) {
        result = x1;
        backPtr_a[t][i] = 1;
      } else {
        result = x0;
        backPtr_a[t][i] = 0;
      }
      alphas_a[curIdxOffset][i] = result + logProbs_a[t][labelIdx];
    }
  }
  auto idx1 = (T - 1) % 2;
  auto ltrIdx = alphas_a[idx1][S - 1] > alphas_a[idx1][S - 2] ? S - 1 : S - 2;
  // path stores the token index for each time step after force alignment.
  auto indexScores = 0;
  for (auto t = T - 1; t > -1; t--) {
    auto lbl_idx = ltrIdx % 2 == 0 ? blank : targets_a[ltrIdx / 2];
    paths_a[t] = lbl_idx;
    ++indexScores;
    ltrIdx -= backPtr_a[t][ltrIdx];
  }
}

std::tuple<torch::Tensor, torch::Tensor> compute(
    const torch::Tensor& logProbs,
    const torch::Tensor& targets,
    const torch::Tensor& inputLengths,
    const torch::Tensor& targetLengths,
    const int64_t blank) {
  TORCH_CHECK(logProbs.is_cpu(), "log_probs must be a CPU tensor");
  TORCH_CHECK(targets.is_cpu(), "targets must be a CPU tensor");
  TORCH_CHECK(
      logProbs.device() == targets.device(),
      "log_probs and targets need to be on the same device");
  TORCH_CHECK(
      logProbs.dtype() == torch::kFloat64 ||
          logProbs.dtype() == torch::kFloat32 ||
          logProbs.dtype() == torch::kFloat16,
      "log_probs must be float64, float32 or float16 (half) type");
  TORCH_CHECK(
      targets.dtype() == torch::kInt32 || targets.dtype() == torch::kInt64,
      "targets must be int32 or int64 type");
  TORCH_CHECK(logProbs.is_contiguous(), "log_probs must be contiguous");
  TORCH_CHECK(targets.is_contiguous(), "targets must be contiguous");
  TORCH_CHECK(
      logProbs.dim() != 3,
      "3-D tensor is not yet supported for log_probs, please provide 2-D tensor.")
  TORCH_CHECK(
      targets.dim() != 2,
      "2-D tensor is not yet supported for targets, please provide 1-D tensor.")
  TORCH_CHECK(
      logProbs.dim() == 2, "log_probs must be 2-D (input length, num classes)");
  TORCH_CHECK(targets.dim() == 1, "targets must be 1-D (target length,)");
  TORCH_CHECK(inputLengths.dim() == 0, "input_lengths must be 0-D");
  TORCH_CHECK(targetLengths.dim() == 0, "target_lengths must be 0-D");
  TORCH_CHECK(
      blank >= 0 && blank < logProbs.size(-1),
      "blank must be within [0, num classes)");

  TORCH_CHECK(
      logProbs.size(0) == at::max(inputLengths).item().toInt(),
      "input length mismatch");
  TORCH_CHECK(
      targets.size(0) == at::max(targetLengths).item().toInt(),
      "target length mismatch");

  const auto T = logProbs.size(0);
  auto paths = torch::zeros(
      {T},
      torch::TensorOptions().device(targets.device()).dtype(targets.dtype()));
  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      logProbs.scalar_type(), "forced_align_impl", [&] {
        if (targets.scalar_type() == torch::kInt64) {
          forced_align_impl<scalar_t, torch::kInt64>(
              logProbs, targets, blank, paths);
        } else {
          forced_align_impl<scalar_t, torch::kInt32>(
              logProbs, targets, blank, paths);
        }
      });
  return std::make_tuple(
      paths,
      logProbs.index(
          {torch::linspace(
               0, T - 1, T, torch::TensorOptions().dtype(paths.dtype())),
           paths}));
}

TORCH_LIBRARY_IMPL(torchaudio, CPU, m) {
  m.impl("forced_align", &compute);
}

} // namespace cpu
} // namespace alignment
} // namespace torchaudio
