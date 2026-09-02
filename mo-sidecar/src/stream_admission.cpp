// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/stream_admission.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace matrixone::sidecar {
namespace {

constexpr std::uint64_t alignment = 64U * 1024U;
constexpr std::uint64_t transport_overhead = 64U * 1024U;
constexpr std::uint64_t expanded_input_bytes = 64U * 1024U * 1024U;

std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw std::overflow_error("stream admission charge overflow");
  }
  return left + right;
}

std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw std::overflow_error("stream admission charge overflow");
  }
  return left * right;
}

std::uint64_t align(std::uint64_t bytes) {
  return checked_multiply(checked_add(bytes, alignment - 1U) / alignment,
                          alignment);
}

} // namespace

std::uint64_t stream_execution_charge(const execute_request &request,
                                      std::size_t stream_inputs) {
  if (stream_inputs > k_max_stream_inputs) {
    throw std::invalid_argument("stream input count exceeds admission bound");
  }
  const auto input_frame =
      align(checked_add(checked_add(request.max_input_batch_bytes,
                                    k_native_batch_frame_header_bytes),
                        transport_overhead));
  const auto per_read =
      checked_add(checked_multiply(2, input_frame), expanded_input_bytes);
  const auto result = checked_multiply(
      2, align(checked_add(request.max_batch_bytes, transport_overhead)));
  const auto request_bytes =
      checked_add(static_cast<std::uint64_t>(request.plan.size()),
                  static_cast<std::uint64_t>(request.result_schema.size()));
  const auto request_overlap = checked_multiply(
      2, align(checked_add(request_bytes, transport_overhead)));
  return checked_add(
      checked_add(checked_multiply(stream_inputs, per_read), result),
      request_overlap);
}

stream_admission_controller::lease::~lease() noexcept { reset(); }

stream_admission_controller::lease::lease(lease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0)) {}

stream_admission_controller::lease &
stream_admission_controller::lease::operator=(lease &&other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0);
  }
  return *this;
}

void stream_admission_controller::lease::reset() noexcept {
  if (owner_) {
    owner_->release(bytes_);
    owner_ = nullptr;
    bytes_ = 0;
  }
}

void stream_admission_controller::lease::shrink(std::uint64_t bytes) {
  if (!owner_ || bytes > bytes_) {
    throw std::invalid_argument("stream admission lease can only shrink");
  }
  owner_->shrink(bytes_, bytes);
  bytes_ = bytes;
}

std::optional<stream_admission_controller::lease>
stream_admission_controller::try_acquire(std::uint64_t bytes) {
  std::lock_guard lock(mutex_);
  if (bytes > capacity_bytes_ - std::min(capacity_bytes_, current_bytes_)) {
    ++rejected_requests_;
    return std::nullopt;
  }
  current_bytes_ += bytes;
  peak_bytes_ = std::max(peak_bytes_, current_bytes_);
  return lease(this, bytes);
}

void stream_admission_controller::release(std::uint64_t bytes) noexcept {
  std::lock_guard lock(mutex_);
  current_bytes_ = bytes > current_bytes_ ? 0 : current_bytes_ - bytes;
}

void stream_admission_controller::shrink(std::uint64_t from, std::uint64_t to) {
  std::lock_guard lock(mutex_);
  if (from < to || from - to > current_bytes_) {
    throw std::logic_error("invalid stream admission shrink");
  }
  current_bytes_ -= from - to;
}

std::uint64_t stream_admission_controller::current_bytes() const noexcept {
  std::lock_guard lock(mutex_);
  return current_bytes_;
}
std::uint64_t stream_admission_controller::peak_bytes() const noexcept {
  std::lock_guard lock(mutex_);
  return peak_bytes_;
}
std::uint64_t stream_admission_controller::rejected_requests() const noexcept {
  std::lock_guard lock(mutex_);
  return rejected_requests_;
}

} // namespace matrixone::sidecar
