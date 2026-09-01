// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "mo_sidecar/protocol.hpp"
#include "offload/mo_native_batch.hpp"
#include "offload/stream_read.hpp"
#include "tae_types.hpp"

#include <arrow/buffer.h>
#include <arrow/result.h>
#include <duckdb/function/table_function.hpp>
#include <substrait/algebra.pb.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace matrixone::sidecar {

using native_column_view = sirius::offload::mo_native_column_view;

class native_batch_view final : public sirius::offload::mo_native_batch {
  public:
	native_batch_view(std::shared_ptr<arrow::Buffer> frame, native_batch_frame envelope,
					  const ::substrait::NamedStruct &schema);

	std::uint64_t sequence() const noexcept override;
	std::uint64_t rows() const noexcept override;
	std::uint64_t payload_bytes() const noexcept override;
	const std::vector<native_column_view> &columns() const noexcept override;

  private:
	std::shared_ptr<arrow::Buffer> frame_;
	std::uint64_t sequence_ = 0;
	std::uint64_t rows_ = 0;
	std::uint64_t payload_bytes_ = 0;
	std::vector<native_column_view> columns_;
};

class stream_input final : public std::enable_shared_from_this<stream_input>,
						   public sirius::offload::mo_native_batch_source {
  public:
	stream_input(sirius::offload::stream_read request, const ::substrait::NamedStruct &schema);

	const sirius::offload::stream_read &request() const noexcept;
	const ::substrait::NamedStruct &canonical_schema() const noexcept;
	const duckdb::vector<duckdb::LogicalType> &types() const noexcept;
	const duckdb::vector<std::string> &names() const noexcept;

	arrow::Status attach();
	arrow::Result<upload_input_ack> publish(std::shared_ptr<arrow::Buffer> frame, const std::function<bool()> &stopped);
	arrow::Result<upload_input_ack> finish_upload(const std::function<bool()> &stopped);
	void detach() noexcept;

	std::shared_ptr<sirius::offload::mo_native_batch> next_batch() override;
	void mark_consumed(std::uint64_t sequence) noexcept override;
	void mark_not_needed() noexcept;
	void cancel(std::string error) noexcept;

  private:
	arrow::Status terminal_status_locked() const;

	sirius::offload::stream_read request_;
	::substrait::NamedStruct canonical_schema_;
	duckdb::vector<duckdb::LogicalType> types_;
	duckdb::vector<std::string> names_;

	std::mutex mutex_;
	std::condition_variable condition_;
	std::shared_ptr<native_batch_view> batch_;
	std::string error_;
	std::uint64_t acknowledged_batches_ = 0;
	std::uint64_t acknowledged_rows_ = 0;
	std::uint64_t acknowledged_bytes_ = 0;
	bool attached_ = false;
	bool detached_ = false;
	bool producer_eof_ = false;
	bool consumer_eof_ = false;
	bool not_needed_ = false;
	bool cancelled_ = false;
};

class stream_input_registry final {
  public:
	std::shared_ptr<stream_input> create(const sirius::offload::stream_read &request,
										 const ::substrait::NamedStruct &schema);
	std::shared_ptr<stream_input> find(const std::string &stream_ref) const;
	void mark_all_not_needed() noexcept;
	void cancel_all(const std::string &error) noexcept;
	std::size_t size() const noexcept;
	std::size_t active_handlers() const noexcept;
	void handler_attached() noexcept;
	void handler_detached() noexcept;

  private:
	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::shared_ptr<stream_input>> inputs_;
	std::atomic<std::size_t> active_handlers_{0};
};

duckdb::TableFunction get_stream_scan_function();

} // namespace matrixone::sidecar
