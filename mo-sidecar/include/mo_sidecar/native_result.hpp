// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "mo_sidecar/protocol.hpp"

#include "offload/substrait_execution.hpp"

#include <cucascade/data/data_batch.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/main/client_context.hpp>

#include <cstddef>
#include <string>

namespace matrixone::sidecar {

void validate_native_result_schema(const native_result_schema &wire, const sirius::offload::execution_schema &actual);

// Encode a non-empty row range using MatrixOne batch/vector MarshalBinary v1.
// The returned bytes are the batch payload; Flight's MOB1 envelope is added by
// the caller so one logical DuckDB chunk may be split into several wire frames.
std::string encode_native_result_batch(const duckdb::DataChunk &chunk, std::size_t row_offset, std::size_t row_count,
									   const native_result_schema &schema);

class native_result_batch_encoder final {
  public:
	native_result_batch_encoder(const std::shared_ptr<cucascade::data_batch> &batch, duckdb::ClientContext &context,
								const native_result_schema &schema, rmm::cuda_stream_view stream);
	~native_result_batch_encoder();
	native_result_batch_encoder(native_result_batch_encoder &&) noexcept;
	native_result_batch_encoder &operator=(native_result_batch_encoder &&) noexcept;
	native_result_batch_encoder(const native_result_batch_encoder &) = delete;
	native_result_batch_encoder &operator=(const native_result_batch_encoder &) = delete;

	std::size_t rows() const noexcept;
	std::size_t encoded_size(std::size_t row_offset, std::size_t row_count) const;
	std::string encode(std::size_t row_offset, std::size_t row_count) const;

  private:
	struct impl;
	std::unique_ptr<impl> impl_;
};

} // namespace matrixone::sidecar
