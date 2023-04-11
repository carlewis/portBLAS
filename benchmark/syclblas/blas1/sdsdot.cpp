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
 *  @filename sdsdot.cpp
 *
 **************************************************************************/

#include "../utils.hpp"

template <typename scalar_t>
std::string get_name(int size) {
  std::ostringstream str{};
  str << "BM_Sdsdot<" << blas_benchmark::utils::get_type_name<scalar_t>()
      << ">/";
  str << size;
  return str.str();
}

template <typename scalar_t>
void run(benchmark::State& state, blas::SB_Handle* sb_handle_ptr, index_t size,
         bool* success) {
  // Google-benchmark counters are double.
  double size_d = static_cast<double>(size);

  state.counters["size"] = size_d;
  state.counters["n_fl_ops"] = 2 * size_d;
  state.counters["bytes_processed"] = 2 * size_d * sizeof(scalar_t);

  blas::SB_Handle& sb_handle = *sb_handle_ptr;

  // Create data
  const float sb = blas_benchmark::utils::random_data<scalar_t>(1)[0];
  std::vector<scalar_t> v1 = blas_benchmark::utils::random_data<scalar_t>(size);
  std::vector<scalar_t> v2 = blas_benchmark::utils::random_data<scalar_t>(size);

  auto inx = blas::make_sycl_iterator_buffer<scalar_t>(v1, size);
  auto iny = blas::make_sycl_iterator_buffer<scalar_t>(v2, size);
  auto inr = blas::make_sycl_iterator_buffer<scalar_t>(1);

#ifdef BLAS_VERIFY_BENCHMARK
  // Run a first time with a verification of the results
  scalar_t vr_ref =
      reference_blas::sdsdot(size, sb, v1.data(), 1, v2.data(), 1);
  scalar_t vr_temp = 0;
  {
    auto vr_temp_gpu = blas::make_sycl_iterator_buffer<scalar_t>(&vr_temp, 1);
    _sdsdot(sb_handle, size, sb, inx, static_cast<index_t>(1), iny,
            static_cast<index_t>(1), vr_temp_gpu);
    auto event = blas::helper::copy_to_host(sb_handle.get_queue(), vr_temp_gpu,
                                            &vr_temp, 1);
    sb_handle.wait(event);
  }

  if (!utils::almost_equal(vr_temp, vr_ref)) {
    std::ostringstream err_stream;
    err_stream << "Value mismatch: " << vr_temp << "; expected " << vr_ref;
    const std::string& err_str = err_stream.str();
    state.SkipWithError(err_str.c_str());
    *success = false;
  };
#endif

  auto blas_method_def = [&]() -> std::vector<cl::sycl::event> {
    auto event = _sdsdot(sb_handle, size, sb, inx, static_cast<index_t>(1), iny,
                         static_cast<index_t>(1), inr);
    sb_handle.wait(event);
    return event;
  };

  // Warmup
  blas_benchmark::utils::warmup(blas_method_def);
  sb_handle.wait();

  blas_benchmark::utils::init_counters(state);

  // Measure
  for (auto _ : state) {
    // Run
    std::tuple<double, double> times =
        blas_benchmark::utils::timef(blas_method_def);

    // Report
    blas_benchmark::utils::update_counters(state, times);
  }

  blas_benchmark::utils::calc_avg_counters(state);
}

template <typename scalar_t>
void register_benchmark(blas_benchmark::Args& args, blas::SB_Handle* sb_handle_ptr,
                        bool* success) {
  auto sdsdot_params = blas_benchmark::utils::get_blas1_params(args);

  for (auto size : sdsdot_params) {
    auto BM_lambda = [&](benchmark::State& st, blas::SB_Handle* sb_handle_ptr,
                         index_t size, bool* success) {
      run<scalar_t>(st, sb_handle_ptr, size, success);
    };
    benchmark::RegisterBenchmark(get_name<scalar_t>(size).c_str(), BM_lambda,
                                 sb_handle_ptr, size, success);
  }
}

namespace blas_benchmark {
void create_benchmark(blas_benchmark::Args& args, blas::SB_Handle* sb_handle_ptr,
                      bool* success) {
  BLAS_REGISTER_BENCHMARK_FLOAT(args, sb_handle_ptr, success);
}
}  // namespace blas_benchmark