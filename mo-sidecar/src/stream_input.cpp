// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/stream_input.hpp"

#include <duckdb/common/types/vector.hpp>

#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

bool sirius::offload::mo_native_column_view::is_null(std::uint64_t row) const noexcept {
	if (null_count == 0 || null_words.empty()) {
		return false;
	}
	const auto offset = static_cast<std::size_t>(row / 64U) * 8U;
	if (offset + 8U > null_words.size()) {
		return false;
	}
	std::uint64_t word = 0;
	for (unsigned i = 0; i < 8; ++i) {
		word |= static_cast<std::uint64_t>(static_cast<unsigned char>(null_words[offset + i])) << (i * 8U);
	}
	return (word & (std::uint64_t{1} << (row % 64U))) != 0;
}

namespace matrixone::sidecar {
namespace {

constexpr std::uint8_t mo_vector_flat = 0;
constexpr std::uint8_t mo_vector_constant = 1;
constexpr std::size_t mo_type_bytes = 16;
constexpr std::size_t mo_bitmap_header_bytes = 24;

class cursor {
  public:
	explicit cursor(std::string_view bytes) : bytes_(bytes) {}

	std::string_view read(std::size_t size) {
		if (size > bytes_.size() - position_) {
			throw std::invalid_argument("truncated MO native batch");
		}
		auto value = bytes_.substr(position_, size);
		position_ += size;
		return value;
	}

	std::uint32_t u32() {
		const auto bytes = read(4);
		std::uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i) {
			value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8U);
		}
		return value;
	}

	std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

	std::uint64_t u64() {
		const auto bytes = read(8);
		std::uint64_t value = 0;
		for (unsigned i = 0; i < 8; ++i) {
			value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8U);
		}
		return value;
	}

	std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

	bool done() const noexcept { return position_ == bytes_.size(); }

  private:
	std::string_view bytes_;
	std::size_t position_ = 0;
};

std::uint64_t load_u64(std::string_view bytes, std::size_t offset) {
	std::uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i) {
		value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
	}
	return value;
}

std::uint32_t physical_rows(const native_column_view &column) {
	if (column.vector_class == mo_vector_constant) {
		return column.data.empty() ? 0U : 1U;
	}
	return column.logical_rows;
}

bool substrait_matches_mo(const ::substrait::Type &expected, const tae::MOType &actual) {
	using oid = tae::MOTypeOid;
	switch (expected.kind_case()) {
	case ::substrait::Type::kBool:
		return actual.oid == oid::MO_T_bool;
	case ::substrait::Type::kI8:
		return actual.oid == oid::MO_T_int8;
	case ::substrait::Type::kI16:
		return actual.oid == oid::MO_T_int16;
	case ::substrait::Type::kI32:
		return actual.oid == oid::MO_T_int32;
	case ::substrait::Type::kI64:
		return actual.oid == oid::MO_T_int64;
	case ::substrait::Type::kFp32:
		return actual.oid == oid::MO_T_float32;
	case ::substrait::Type::kFp64:
		return actual.oid == oid::MO_T_float64;
	case ::substrait::Type::kString:
	case ::substrait::Type::kVarchar:
		return actual.oid == oid::MO_T_char || actual.oid == oid::MO_T_varchar;
	case ::substrait::Type::kDate:
		return actual.oid == oid::MO_T_date;
	case ::substrait::Type::kDecimal:
		return (expected.decimal().precision() <= 18 ? actual.oid == oid::MO_T_decimal64
													 : actual.oid == oid::MO_T_decimal128) &&
			   actual.width == expected.decimal().precision() && actual.scale == expected.decimal().scale();
	default:
		return false;
	}
}

duckdb::LogicalType logical_type(const ::substrait::Type &type) {
	switch (type.kind_case()) {
	case ::substrait::Type::kBool:
		return duckdb::LogicalType::BOOLEAN;
	case ::substrait::Type::kI8:
		return duckdb::LogicalType::TINYINT;
	case ::substrait::Type::kI16:
		return duckdb::LogicalType::SMALLINT;
	case ::substrait::Type::kI32:
		return duckdb::LogicalType::INTEGER;
	case ::substrait::Type::kI64:
		return duckdb::LogicalType::BIGINT;
	case ::substrait::Type::kFp32:
		return duckdb::LogicalType::FLOAT;
	case ::substrait::Type::kFp64:
		return duckdb::LogicalType::DOUBLE;
	case ::substrait::Type::kString:
	case ::substrait::Type::kVarchar:
		return duckdb::LogicalType::VARCHAR;
	case ::substrait::Type::kDate:
		return duckdb::LogicalType::DATE;
	case ::substrait::Type::kDecimal:
		return duckdb::LogicalType::DECIMAL(type.decimal().precision(), type.decimal().scale());
	default:
		throw std::invalid_argument("unsupported StreamRead logical type");
	}
}

native_column_view parse_column(std::string_view bytes, std::uint64_t batch_rows, const ::substrait::Type &expected) {
	cursor input(bytes);
	native_column_view result;
	result.encoded = bytes;
	result.vector_class = static_cast<std::uint8_t>(input.read(1)[0]);
	if (result.vector_class != mo_vector_flat && result.vector_class != mo_vector_constant) {
		throw std::invalid_argument("unsupported MO vector class");
	}
	const auto type_bytes = input.read(mo_type_bytes);
	std::memcpy(&result.type, type_bytes.data(), sizeof(result.type));
	if (!substrait_matches_mo(expected, result.type)) {
		throw std::invalid_argument("MO vector type does not match StreamRead schema");
	}
	result.logical_rows = input.u32();
	if (result.logical_rows != batch_rows) {
		throw std::invalid_argument("MO vector length does not match batch row count");
	}
	result.data = input.read(input.u32());
	result.area = input.read(input.u32());
	const auto nulls = input.read(input.u32());
	const auto sorted = static_cast<std::uint8_t>(input.read(1)[0]);
	if (!input.done() || sorted > 1) {
		throw std::invalid_argument("invalid or trailing MO vector data");
	}
	result.sorted = sorted != 0;

	const auto fixed_size = tae::MOTypeFixedSize(static_cast<tae::MOTypeOid>(result.type.oid));
	const auto element_size = fixed_size < 0 ? static_cast<std::int32_t>(tae::VARLENA_SIZE) : fixed_size;
	const std::uint64_t expected_data = result.vector_class == mo_vector_constant
											? (result.data.empty() ? 0U : static_cast<std::uint64_t>(element_size))
											: batch_rows * static_cast<std::uint64_t>(element_size);
	if (expected_data != result.data.size() || (fixed_size >= 0 && !result.area.empty())) {
		throw std::invalid_argument("invalid MO vector data or area length");
	}

	if (!nulls.empty()) {
		if (nulls.size() < mo_bitmap_header_bytes) {
			throw std::invalid_argument("truncated MO null bitmap");
		}
		const auto count = static_cast<std::int64_t>(load_u64(nulls, 0));
		const auto bit_length = load_u64(nulls, 8);
		const auto word_bytes = load_u64(nulls, 16);
		if (count < 0 || static_cast<std::uint64_t>(count) > batch_rows || bit_length < batch_rows ||
			bit_length > std::numeric_limits<std::uint32_t>::max() ||
			word_bytes != nulls.size() - mo_bitmap_header_bytes || word_bytes % 8 != 0 ||
			word_bytes / 8 != (bit_length + 63) / 64) {
			throw std::invalid_argument("invalid MO null bitmap header");
		}
		result.null_count = static_cast<std::uint64_t>(count);
		result.null_words = nulls.substr(mo_bitmap_header_bytes);
		std::uint64_t actual = 0;
		for (std::size_t offset = 0; offset < result.null_words.size(); offset += 8) {
			const auto word = load_u64(result.null_words, offset);
			actual += static_cast<std::uint64_t>(std::popcount(word));
			const auto first_row = offset / 8 * 64;
			const auto remaining = first_row < batch_rows ? batch_rows - first_row : 0;
			const auto allowed = remaining >= 64 ? std::numeric_limits<std::uint64_t>::max()
												 : (remaining == 0 ? 0 : (std::uint64_t{1} << remaining) - 1);
			if ((word & ~allowed) != 0) {
				throw std::invalid_argument("MO null bitmap contains rows outside the batch");
			}
		}
		if (actual != result.null_count) {
			throw std::invalid_argument("MO null bitmap count mismatch");
		}
	}

	if (fixed_size < 0) {
		const auto rows = physical_rows(result);
		if (result.data.size() != static_cast<std::size_t>(rows) * tae::VARLENA_SIZE) {
			throw std::invalid_argument("invalid MO varlena descriptor length");
		}
		for (std::uint32_t row = 0; row < rows; ++row) {
			tae::Varlena value{};
			std::memcpy(&value, result.data.data() + row * tae::VARLENA_SIZE, sizeof(value));
			if (value.is_inline()) {
				continue;
			}
			std::uint32_t marker = 0;
			std::memcpy(&marker, value.data, sizeof(marker));
			const auto offset = value.big_offset();
			const auto length = value.big_length();
			if (marker != tae::VARLENA_BIG_MARKER || offset > result.area.size() ||
				length > result.area.size() - offset) {
				throw std::invalid_argument("MO varlena descriptor is outside its area");
			}
		}
	}
	return result;
}

void fill_column(duckdb::Vector &output, const native_column_view &column, duckdb::idx_t count,
				 duckdb::idx_t source_offset) {
	const bool constant = column.vector_class == mo_vector_constant;
	if (constant) {
		output.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
		duckdb::ConstantVector::SetNull(output, false);
		if (column.data.empty() || column.is_null(0)) {
			duckdb::ConstantVector::SetNull(output, true);
			return;
		}
	} else {
		output.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
		duckdb::FlatVector::Validity(output).SetAllValid(count);
	}
	const auto source_row = [&](duckdb::idx_t row) { return constant ? 0U : source_offset + row; };
	const auto oid = static_cast<tae::MOTypeOid>(column.type.oid);

	if (oid == tae::MO_T_char || oid == tae::MO_T_varchar) {
		auto *target = constant ? duckdb::ConstantVector::GetData<duckdb::string_t>(output)
								: duckdb::FlatVector::GetData<duckdb::string_t>(output);
		const auto rows = constant ? 1U : count;
		for (duckdb::idx_t row = 0; row < rows; ++row) {
			const auto index = source_row(row);
			if (column.is_null(index)) {
				continue;
			}
			tae::Varlena value{};
			std::memcpy(&value, column.data.data() + index * tae::VARLENA_SIZE, sizeof(value));
			const char *data = value.inline_data();
			std::uint32_t length = value.inline_length();
			if (!value.is_inline()) {
				data = column.area.data() + value.big_offset();
				length = value.big_length();
			}
			target[constant ? 0 : row] = duckdb::StringVector::AddString(output, data, length);
		}
	} else if (oid == tae::MO_T_date) {
		auto *target = constant ? duckdb::ConstantVector::GetData<std::int32_t>(output)
								: duckdb::FlatVector::GetData<std::int32_t>(output);
		const auto rows = constant ? 1U : count;
		for (duckdb::idx_t row = 0; row < rows; ++row) {
			std::int32_t value = 0;
			std::memcpy(&value, column.data.data() + source_row(row) * sizeof(value), sizeof(value));
			target[constant ? 0 : row] = value - tae::MO_UNIX_EPOCH_DAYS;
		}
	} else {
		const auto width = tae::MOTypeFixedSize(oid);
		if (width <= 0) {
			throw std::invalid_argument("unsupported MO stream vector type");
		}
		auto *target = constant ? duckdb::ConstantVector::GetData(output) : duckdb::FlatVector::GetData(output);
		const auto rows = constant ? 1U : count;
		const auto offset = constant ? 0U : source_offset;
		std::memcpy(target, column.data.data() + offset * width, rows * width);
	}

	if (!constant && column.null_count != 0) {
		auto &validity = duckdb::FlatVector::Validity(output);
		for (duckdb::idx_t row = 0; row < count; ++row) {
			if (column.is_null(source_offset + row)) {
				validity.SetInvalid(row);
			}
		}
	}
}

struct stream_global_state final : public duckdb::GlobalTableFunctionState {
	explicit stream_global_state(std::shared_ptr<stream_input> input) : input(std::move(input)) {}
	duckdb::idx_t MaxThreads() const override { return 1; }
	std::shared_ptr<stream_input> input;
	std::shared_ptr<sirius::offload::mo_native_batch> batch;
	duckdb::idx_t offset = 0;
};

duckdb::unique_ptr<duckdb::FunctionData> bind_stream(duckdb::ClientContext &, duckdb::TableFunctionBindInput &input,
													 duckdb::vector<duckdb::LogicalType> &return_types,
													 duckdb::vector<std::string> &names) {
	auto *raw = reinterpret_cast<stream_input *>(input.inputs.at(0).GetPointer());
	if (!raw) {
		throw std::invalid_argument("mo_stream_scan input is null");
	}
	auto shared = raw->shared_from_this();
	return_types = shared->types();
	names = shared->names();
	return duckdb::make_uniq<sirius::offload::mo_native_scan_bind_data>(std::move(shared));
}

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> init_stream(duckdb::ClientContext &,
																 duckdb::TableFunctionInitInput &input) {
	auto source = std::dynamic_pointer_cast<stream_input>(
		input.bind_data->Cast<sirius::offload::mo_native_scan_bind_data>().source);
	if (!source) {
		throw std::invalid_argument("mo_stream_scan bind data is not a sidecar stream input");
	}
	return duckdb::make_uniq<stream_global_state>(std::move(source));
}

void scan_stream(duckdb::ClientContext &, duckdb::TableFunctionInput &input, duckdb::DataChunk &output) {
	auto &state = input.global_state->Cast<stream_global_state>();
	if (state.batch && state.offset == state.batch->rows()) {
		state.input->mark_consumed(state.batch->sequence());
		state.batch.reset();
		state.offset = 0;
	}
	if (!state.batch) {
		state.batch = state.input->next_batch();
		if (!state.batch) {
			return;
		}
	}
	const auto count =
		static_cast<duckdb::idx_t>(std::min<std::uint64_t>(STANDARD_VECTOR_SIZE, state.batch->rows() - state.offset));
	for (duckdb::idx_t column = 0; column < state.batch->columns().size(); ++column) {
		fill_column(output.data[column], state.batch->columns()[column], count, state.offset);
	}
	output.SetCardinality(count);
	state.offset += count;
}

} // namespace

native_batch_view::native_batch_view(std::shared_ptr<arrow::Buffer> frame, native_batch_frame envelope,
									 const ::substrait::NamedStruct &schema)
	: frame_(std::move(frame)), sequence_(envelope.sequence), payload_bytes_(envelope.payload.size()) {
	if (std::endian::native != std::endian::little || sizeof(tae::MOType) != mo_type_bytes ||
		sizeof(tae::Varlena) != tae::VARLENA_SIZE) {
		throw std::invalid_argument("MO native batch ABI is unsupported on this sidecar");
	}
	cursor input(envelope.payload);
	const auto signed_rows = input.i64();
	if (signed_rows < 0 || static_cast<std::uint64_t>(signed_rows) > std::numeric_limits<std::uint32_t>::max()) {
		throw std::invalid_argument("invalid MO batch row count");
	}
	rows_ = static_cast<std::uint64_t>(signed_rows);
	if (rows_ == 0) {
		throw std::invalid_argument("empty MO native batches must not be uploaded");
	}
	const auto vector_count = input.i32();
	if (vector_count < 0 || vector_count != schema.names_size() || vector_count != schema.struct_().types_size()) {
		throw std::invalid_argument("MO batch vector count does not match StreamRead schema");
	}
	columns_.reserve(vector_count);
	for (int column = 0; column < vector_count; ++column) {
		columns_.push_back(parse_column(input.read(input.u32()), rows_, schema.struct_().types(column)));
	}
	const auto attribute_count = input.i32();
	if (attribute_count < 0 || (attribute_count != 0 && attribute_count != vector_count)) {
		throw std::invalid_argument("invalid MO batch attribute count");
	}
	for (int attribute = 0; attribute < attribute_count; ++attribute) {
		const auto size = input.i32();
		if (size < 0) {
			throw std::invalid_argument("invalid MO batch attribute length");
		}
		(void)input.read(static_cast<std::size_t>(size));
	}
	const auto extra = input.i32();
	if (extra != 0 || input.i32() != 0 || input.i32() != 0 || !input.done()) {
		throw std::invalid_argument("unsupported or trailing MO batch metadata");
	}
}

std::uint64_t native_batch_view::sequence() const noexcept { return sequence_; }
std::uint64_t native_batch_view::rows() const noexcept { return rows_; }
std::uint64_t native_batch_view::payload_bytes() const noexcept { return payload_bytes_; }
const std::vector<native_column_view> &native_batch_view::columns() const noexcept { return columns_; }

stream_input::stream_input(sirius::offload::stream_read request, const ::substrait::NamedStruct &schema)
	: request_(std::move(request)), canonical_schema_(schema) {
	if (!schema.has_struct_() || schema.names_size() == 0 || schema.names_size() != schema.struct_().types_size()) {
		throw std::invalid_argument("StreamRead schema is incomplete");
	}
	for (int column = 0; column < schema.names_size(); ++column) {
		names_.push_back(schema.names(column));
		types_.push_back(logical_type(schema.struct_().types(column)));
	}
}

const sirius::offload::stream_read &stream_input::request() const noexcept { return request_; }
const ::substrait::NamedStruct &stream_input::canonical_schema() const noexcept { return canonical_schema_; }
const duckdb::vector<duckdb::LogicalType> &stream_input::types() const noexcept { return types_; }
const duckdb::vector<std::string> &stream_input::names() const noexcept { return names_; }

arrow::Status stream_input::attach() {
	std::lock_guard lock(mutex_);
	if (attached_) {
		return arrow::Status::Invalid("StreamRead input was already attached");
	}
	if (cancelled_) {
		return terminal_status_locked();
	}
	attached_ = true;
	detached_ = false;
	return arrow::Status::OK();
}

arrow::Result<upload_input_ack> stream_input::publish(std::shared_ptr<arrow::Buffer> frame,
													  const std::function<bool()> &stopped) {
	if (!frame) {
		return arrow::Status::Invalid("MO native batch frame is empty");
	}
	const std::string_view bytes(reinterpret_cast<const char *>(frame->data()), frame->size());
	auto envelope = parse_native_batch_frame(bytes);
	auto decoded = std::make_shared<native_batch_view>(frame, envelope, canonical_schema_);
	std::unique_lock lock(mutex_);
	if (!attached_ || detached_ || producer_eof_) {
		return arrow::Status::Invalid("StreamRead input is not writable");
	}
	if (envelope.sequence != acknowledged_batches_ + 1) {
		return arrow::Status::Invalid("MO native batch sequence is not contiguous");
	}
	while (batch_ && !cancelled_ && !not_needed_) {
		lock.unlock();
		const bool caller_stopped = stopped && stopped();
		lock.lock();
		if (caller_stopped) {
			return arrow::Status::Cancelled("StreamRead upload was cancelled");
		}
		condition_.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (cancelled_) {
		return terminal_status_locked();
	}
	if (not_needed_) {
		return upload_input_ack{acknowledged_batches_, acknowledged_rows_, acknowledged_bytes_, true, true};
	}
	batch_ = std::move(decoded);
	condition_.notify_all();
	while (batch_ && !cancelled_ && !not_needed_) {
		lock.unlock();
		const bool caller_stopped = stopped && stopped();
		lock.lock();
		if (caller_stopped) {
			return arrow::Status::Cancelled("StreamRead upload was cancelled");
		}
		condition_.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (cancelled_) {
		return terminal_status_locked();
	}
	return upload_input_ack{acknowledged_batches_, acknowledged_rows_, acknowledged_bytes_, not_needed_, not_needed_};
}

arrow::Result<upload_input_ack> stream_input::finish_upload(const std::function<bool()> &stopped) {
	std::unique_lock lock(mutex_);
	producer_eof_ = true;
	condition_.notify_all();
	while (!consumer_eof_ && !cancelled_ && !not_needed_) {
		lock.unlock();
		const bool caller_stopped = stopped && stopped();
		lock.lock();
		if (caller_stopped) {
			return arrow::Status::Cancelled("StreamRead upload was cancelled");
		}
		condition_.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (cancelled_) {
		return terminal_status_locked();
	}
	return upload_input_ack{acknowledged_batches_, acknowledged_rows_, acknowledged_bytes_, true, not_needed_};
}

void stream_input::detach() noexcept {
	std::lock_guard lock(mutex_);
	detached_ = true;
	condition_.notify_all();
}

std::shared_ptr<sirius::offload::mo_native_batch> stream_input::next_batch() {
	std::unique_lock lock(mutex_);
	condition_.wait(lock, [&] { return batch_ || producer_eof_ || cancelled_ || not_needed_; });
	if (cancelled_) {
		throw std::runtime_error(error_.empty() ? "StreamRead input was cancelled" : error_);
	}
	if (not_needed_) {
		return nullptr;
	}
	if (!batch_) {
		consumer_eof_ = true;
		condition_.notify_all();
		return nullptr;
	}
	return batch_;
}

void stream_input::mark_consumed(std::uint64_t sequence) noexcept {
	std::lock_guard lock(mutex_);
	if (!batch_ || batch_->sequence() != sequence) {
		return;
	}
	acknowledged_batches_++;
	acknowledged_rows_ += batch_->rows();
	acknowledged_bytes_ += batch_->payload_bytes();
	batch_.reset();
	condition_.notify_all();
}

void stream_input::mark_not_needed() noexcept {
	std::lock_guard lock(mutex_);
	if (!cancelled_ && !consumer_eof_) {
		not_needed_ = true;
		batch_.reset();
	}
	condition_.notify_all();
}

void stream_input::cancel(std::string error) noexcept {
	std::lock_guard lock(mutex_);
	if (!cancelled_ && !not_needed_) {
		cancelled_ = true;
		error_ = std::move(error);
		batch_.reset();
	}
	condition_.notify_all();
}

arrow::Status stream_input::terminal_status_locked() const {
	return arrow::Status::Cancelled(error_.empty() ? "StreamRead input was cancelled" : error_);
}

std::shared_ptr<stream_input> stream_input_registry::create(const sirius::offload::stream_read &request,
															const ::substrait::NamedStruct &schema) {
	auto input = std::make_shared<stream_input>(request, schema);
	std::lock_guard lock(mutex_);
	if (inputs_.size() >= k_max_stream_inputs) {
		throw std::invalid_argument("StreamRead input count exceeds the supported bound");
	}
	if (!inputs_.emplace(request.stream_ref, input).second) {
		throw std::invalid_argument("duplicate StreamRead input identity");
	}
	return input;
}

std::shared_ptr<stream_input> stream_input_registry::find(const std::string &stream_ref) const {
	std::lock_guard lock(mutex_);
	const auto found = inputs_.find(stream_ref);
	return found == inputs_.end() ? nullptr : found->second;
}

void stream_input_registry::mark_all_not_needed() noexcept {
	std::vector<std::shared_ptr<stream_input>> inputs;
	{
		std::lock_guard lock(mutex_);
		for (const auto &[_, input] : inputs_) {
			inputs.push_back(input);
		}
	}
	for (const auto &input : inputs) {
		input->mark_not_needed();
	}
}

void stream_input_registry::cancel_all(const std::string &error) noexcept {
	std::vector<std::shared_ptr<stream_input>> inputs;
	{
		std::lock_guard lock(mutex_);
		for (const auto &[_, input] : inputs_) {
			inputs.push_back(input);
		}
	}
	for (const auto &input : inputs) {
		input->cancel(error);
	}
}

std::size_t stream_input_registry::size() const noexcept {
	std::lock_guard lock(mutex_);
	return inputs_.size();
}
std::size_t stream_input_registry::active_handlers() const noexcept { return active_handlers_.load(); }
void stream_input_registry::handler_attached() noexcept { active_handlers_.fetch_add(1); }
void stream_input_registry::handler_detached() noexcept { active_handlers_.fetch_sub(1); }

duckdb::TableFunction get_stream_scan_function() {
	duckdb::TableFunction function("mo_stream_scan", {duckdb::LogicalType::POINTER}, scan_stream, bind_stream,
								   init_stream);
	function.projection_pushdown = false;
	function.filter_pushdown = false;
	return function;
}

} // namespace matrixone::sidecar
