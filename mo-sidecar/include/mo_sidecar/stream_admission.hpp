// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "mo_sidecar/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace matrixone::sidecar {

std::uint64_t stream_execution_charge(const execute_request &request,
                                      std::size_t stream_inputs);

class stream_admission_controller final {
public:
  class lease final {
  public:
    lease() = default;
    ~lease() noexcept;
    lease(lease &&other) noexcept;
    lease &operator=(lease &&other) noexcept;
    lease(const lease &) = delete;
    lease &operator=(const lease &) = delete;

    std::uint64_t bytes() const noexcept { return bytes_; }
    void shrink(std::uint64_t bytes);

  private:
    friend class stream_admission_controller;
    lease(stream_admission_controller *owner, std::uint64_t bytes)
        : owner_(owner), bytes_(bytes) {}
    void reset() noexcept;
    stream_admission_controller *owner_ = nullptr;
    std::uint64_t bytes_ = 0;
  };

  explicit stream_admission_controller(std::uint64_t capacity_bytes)
      : capacity_bytes_(capacity_bytes) {}
  std::optional<lease> try_acquire(std::uint64_t bytes);
  std::uint64_t current_bytes() const noexcept;
  std::uint64_t peak_bytes() const noexcept;
  std::uint64_t rejected_requests() const noexcept;

private:
  void release(std::uint64_t bytes) noexcept;
  void shrink(std::uint64_t from, std::uint64_t to);

  const std::uint64_t capacity_bytes_;
  mutable std::mutex mutex_;
  std::uint64_t current_bytes_ = 0;
  std::uint64_t peak_bytes_ = 0;
  std::uint64_t rejected_requests_ = 0;
};

} // namespace matrixone::sidecar
