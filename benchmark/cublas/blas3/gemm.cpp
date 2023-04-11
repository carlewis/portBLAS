/**************************************************************************
 *
 *  @license
 *  Copyright (C) Codeplay Software Limited
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  For your convenience, a copy of the License has been included in this
 *  repository.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  SYCL-BLAS: BLAS implementation using SYCL
 *
 *  @filename gemm.cpp
 *
 **************************************************************************/

#include "../utils.hpp"

template <typename scalar_t>
std::string get_name(std::string t1, std::string t2, int m, int k, int n) {
  std::ostringstream str{};
  str << "BM_Gemm<" << blas_benchmark::utils::get_type_name<scalar_t>() << ">/"
      << t1 << "/" << t2 << "/" << m << "/" << k << "/" << n;
  return str.str();
}

template <typename scalar_t, typename... args_t>
static inline void cublas_routine(args_t&&... args) {
  if constexpr (std::is_same_v<scalar_t, float>) {
    CUBLAS_CHECK(cublasSgemm(std::forward<args_t>(args)...));
  } else if constexpr (std::is_same_v<scalar_t, double>) {
    CUBLAS_CHECK(cublasDgemm(std::forward<args_t>(args)...));
  }
  return;
}

template <typename scalar_t>
void run(benchmark::State& state, cublasHandle_t* cuda_handle_ptr, int t1,
         int t2, index_t m, index_t k, index_t n, scalar_t alpha, scalar_t beta,
         bool* success) {
  // Standard test setup.
  std::string t1s = blas_benchmark::utils::from_transpose_enum(
      static_cast<blas_benchmark::utils::Transposition>(t1));
  std::string t2s = blas_benchmark::utils::from_transpose_enum(
      static_cast<blas_benchmark::utils::Transposition>(t2));
  const char* t_a = t1s.c_str();
  const char* t_b = t2s.c_str();

  index_t lda = t_a[0] == 'n' ? m : k;
  index_t ldb = t_b[0] == 'n' ? k : n;
  index_t ldc = m;

  cublasHandle_t& cuda_handle = *cuda_handle_ptr;

  // Matrices
  std::vector<scalar_t> a = blas_benchmark::utils::random_data<scalar_t>(m * k);
  std::vector<scalar_t> b = blas_benchmark::utils::random_data<scalar_t>(k * n);
  std::vector<scalar_t> c =
      blas_benchmark::utils::const_data<scalar_t>(m * n, 0);

  blas_benchmark::utils::CUDAVector<scalar_t> a_gpu(m * k, a.data());
  blas_benchmark::utils::CUDAVector<scalar_t> b_gpu(k * n, b.data());
  blas_benchmark::utils::CUDAVector<scalar_t> c_gpu(n * m, c.data());

  cublasOperation_t c_t_a = (*t_a == 'n') ? CUBLAS_OP_N : CUBLAS_OP_T;
  cublasOperation_t c_t_b = (*t_b == 'n') ? CUBLAS_OP_N : CUBLAS_OP_T;

#ifdef BLAS_VERIFY_BENCHMARK
  // Run a first time with a verification of the results
  std::vector<scalar_t> c_ref = c;
  reference_blas::gemm(t_a, t_b, m, n, k, alpha, a.data(), lda, b.data(), ldb,
                       beta, c_ref.data(), ldc);
  std::vector<scalar_t> c_temp = c;
  {
    blas_benchmark::utils::CUDAVector<scalar_t, true> c_temp_gpu(m * n,
                                                                 c_temp.data());
    cublas_routine<scalar_t>(cuda_handle, c_t_a, c_t_b, m, n, k, &alpha, a_gpu,
                             lda, b_gpu, ldb, &beta, c_temp_gpu, ldc);
  }

  std::ostringstream err_stream;
  if (!utils::compare_vectors(c_temp, c_ref, err_stream, "")) {
    const std::string& err_str = err_stream.str();
    state.SkipWithError(err_str.c_str());
    *success = false;
  };
#endif
  auto blas_warmup = [&]() -> void {
    cublas_routine<scalar_t>(cuda_handle, c_t_a, c_t_b, m, n, k, &alpha, a_gpu,
                             lda, b_gpu, ldb, &beta, c_gpu, ldc);
    return;
  };

  cudaEvent_t start;
  cudaEvent_t stop;
  CUDA_CHECK(cudaEventCreate(&start));
  CUDA_CHECK(cudaEventCreate(&stop));

  auto blas_method_def = [&]() -> std::vector<cudaEvent_t> {
    CUDA_CHECK(cudaEventRecord(start));
    cublas_routine<scalar_t>(cuda_handle, c_t_a, c_t_b, m, n, k, &alpha, a_gpu,
                             lda, b_gpu, ldb, &beta, c_gpu, ldc);
    CUDA_CHECK(cudaEventRecord(stop));
    CUDA_CHECK(cudaEventSynchronize(stop));
    return std::vector{start, stop};
  };

  // Warmup
  blas_benchmark::utils::warmup(blas_warmup);
  CUDA_CHECK(cudaStreamSynchronize(NULL));

  blas_benchmark::utils::init_counters(state);

  // Measure
  for (auto _ : state) {
    // Run
    std::tuple<double, double> times =
        blas_benchmark::utils::timef_cuda(blas_method_def);

    // Report
    blas_benchmark::utils::update_counters(state, times);
  }

  {
    // The counters are double. We convert m, n and k to double to avoid
    // integer overflows for n_fl_ops and bytes_processed
    double m_d = static_cast<double>(m);
    double n_d = static_cast<double>(n);
    double k_d = static_cast<double>(k);

    state.counters["m"] = m_d;
    state.counters["k"] = k_d;
    state.counters["n"] = n_d;

    double mem_readA = m_d * k_d;
    double mem_readB = k_d * n_d;
    double mem_writeC = m_d * n_d;
    double mem_readC = (beta != scalar_t{0}) ? m_d * n_d : 0;
    double total_mem =
        (mem_readA + mem_readB + mem_readC + mem_writeC) * sizeof(scalar_t);
    state.counters["bytes_processed"] = total_mem;
    state.SetBytesProcessed(state.iterations() * total_mem);

    double nflops_AtimesB = (2 * k_d) * m_d * n_d;
    double nflops_addBetaC = (beta != scalar_t{0}) ? 2 * m_d * n_d : 0;
    double nflops = nflops_AtimesB + nflops_addBetaC;
    state.counters["n_fl_ops"] = nflops;
    state.SetItemsProcessed(state.iterations() * nflops);
  }
  blas_benchmark::utils::calc_avg_counters(state);

  CUDA_CHECK(cudaEventDestroy(start));
  CUDA_CHECK(cudaEventDestroy(stop));
};

template <typename scalar_t>
void register_benchmark(blas_benchmark::Args& args,
                        cublasHandle_t* cuda_handle_ptr, bool* success) {
  auto gemm_params = blas_benchmark::utils::get_blas3_params<scalar_t>(args);

  for (auto p : gemm_params) {
    std::string t1s, t2s;
    index_t m, n, k;
    scalar_t alpha, beta;
    std::tie(t1s, t2s, m, k, n, alpha, beta) = p;
    int t1 = static_cast<int>(blas_benchmark::utils::to_transpose_enum(t1s));
    int t2 = static_cast<int>(blas_benchmark::utils::to_transpose_enum(t2s));

    auto BM_lambda = [&](benchmark::State& st, cublasHandle_t* cuda_handle_ptr,
                         int t1, int t2, index_t m, index_t k, index_t n,
                         scalar_t alpha, scalar_t beta, bool* success) {
      run<scalar_t>(st, cuda_handle_ptr, t1, t2, m, k, n, alpha, beta, success);
    };
    benchmark::RegisterBenchmark(get_name<scalar_t>(t1s, t2s, m, k, n).c_str(),
                                 BM_lambda, cuda_handle_ptr, t1, t2, m, k, n,
                                 alpha, beta, success)
        ->UseRealTime();
  }
}

namespace blas_benchmark {
void create_benchmark(blas_benchmark::Args& args,
                      cublasHandle_t* cuda_handle_ptr, bool* success) {
  BLAS_REGISTER_BENCHMARK(args, cuda_handle_ptr, success);
}
}  // namespace blas_benchmark