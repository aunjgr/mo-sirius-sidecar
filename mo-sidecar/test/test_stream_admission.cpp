// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "catch.hpp"

#include "mo_sidecar/stream_admission.hpp"

using matrixone::sidecar::execute_request;
using matrixone::sidecar::stream_admission_controller;
using matrixone::sidecar::stream_execution_charge;

namespace {

execute_request request() {
  execute_request result;
  result.max_input_batch_bytes = 4U * 1024U * 1024U;
  result.max_batch_bytes = 64U * 1024U * 1024U;
  result.plan.assign(1024, 'p');
  result.result_schema.assign(512, 's');
  return result;
}

} // namespace

TEST_CASE("Stream admission is fail-closed and returns to zero",
          "[sidecar][admission]") {
  const auto maximum = stream_execution_charge(
      request(), matrixone::sidecar::k_max_stream_inputs);
  const auto actual = stream_execution_charge(request(), 2);
  REQUIRE(actual < maximum);

  stream_admission_controller controller(maximum);
  auto lease = controller.try_acquire(maximum);
  REQUIRE(lease.has_value());
  REQUIRE(controller.current_bytes() == maximum);
  REQUIRE_FALSE(controller.try_acquire(1).has_value());
  REQUIRE(controller.rejected_requests() == 1);

  lease->shrink(actual);
  REQUIRE(controller.current_bytes() == actual);
  lease.reset();
  REQUIRE(controller.current_bytes() == 0);
  REQUIRE(controller.peak_bytes() == maximum);
}

TEST_CASE("Stream admission rejects invalid resize and input count",
          "[sidecar][admission]") {
  REQUIRE_THROWS(stream_execution_charge(
      request(), matrixone::sidecar::k_max_stream_inputs + 1));
  stream_admission_controller controller(1024);
  auto lease = controller.try_acquire(512);
  REQUIRE(lease.has_value());
  REQUIRE_THROWS(lease->shrink(513));
  REQUIRE(controller.current_bytes() == 512);
}
