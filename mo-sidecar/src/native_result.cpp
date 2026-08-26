// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/native_result.hpp"

#include "tae_types.hpp"

#include "data/sirius_converter_registry.hpp"
#include "offload/mo_native_result_pack.hpp"
#include "sirius_context.hpp"

#include <cucascade/data/cpu_data_representation.hpp>
#include <cucascade/data/gpu_data_representation.hpp>
#include <cucascade/memory/common.hpp>

#include <duckdb/common/types/vector.hpp>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace matrixone::sidecar {
namespace {

constexpr std::size_t mo_type_bytes = 16;
constexpr std::size_t mo_varlena_bytes = 24;

static_assert(std::endian::native == std::endian::little, "MO native result codec requires little endian");
static_assert(sizeof(tae::MOType) == mo_type_bytes);
static_assert(sizeof(tae::Varlena) == mo_varlena_bytes);

template <typename T> void append_scalar(std::string &output, T value) {
	static_assert(std::is_trivially_copyable_v<T>);
	output.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void append_u32(std::string &output, std::size_t value, std::string_view what) {
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		throw std::overflow_error(std::string(what) + " exceeds the MO native batch format");
	}
	append_scalar(output, static_cast<std::uint32_t>(value));
}

std::int32_t mo_type_size(std::uint32_t oid) {
	const auto fixed = tae::MOTypeFixedSize(static_cast<tae::MOTypeOid>(oid));
	return fixed < 0 ? static_cast<std::int32_t>(tae::VARLENA_SIZE) : fixed;
}

bool is_text(std::uint32_t oid) { return oid == tae::MO_T_char || oid == tae::MO_T_varchar; }

duckdb::LogicalType expected_duckdb_type(const native_result_column &column) {
	switch (column.oid) {
	case tae::MO_T_bool:
		return duckdb::LogicalType::BOOLEAN;
	case tae::MO_T_int8:
		return duckdb::LogicalType::TINYINT;
	case tae::MO_T_int16:
		return duckdb::LogicalType::SMALLINT;
	case tae::MO_T_int32:
		return duckdb::LogicalType::INTEGER;
	case tae::MO_T_int64:
	case tae::MO_T_uint32:
		// MatrixOne exports uint32 through Substrait i64. Narrow only after
		// checking the value when the result is encoded.
		return duckdb::LogicalType::BIGINT;
	case tae::MO_T_float32:
		return duckdb::LogicalType::FLOAT;
	case tae::MO_T_float64:
		return duckdb::LogicalType::DOUBLE;
	case tae::MO_T_decimal64:
	case tae::MO_T_decimal128:
		return duckdb::LogicalType::DECIMAL(column.width, column.scale);
	case tae::MO_T_date:
		return duckdb::LogicalType::DATE;
	case tae::MO_T_char:
	case tae::MO_T_varchar:
		return duckdb::LogicalType::VARCHAR;
	default:
		throw std::invalid_argument("unsupported MO native result OID " + std::to_string(column.oid));
	}
}

void validate_column(const native_result_column &wire, const duckdb::LogicalType &actual, std::size_t index) {
	const auto expected = expected_duckdb_type(wire);
	const bool decimal_from_double =
		(wire.oid == tae::MO_T_decimal64 || wire.oid == tae::MO_T_decimal128) && actual == duckdb::LogicalType::DOUBLE;
	const bool decimal128_from_hugeint =
		wire.oid == tae::MO_T_decimal128 && wire.scale == 0 && actual == duckdb::LogicalType::HUGEINT;
	if (actual != expected && !decimal_from_double && !decimal128_from_hugeint) {
		throw std::invalid_argument("native result column " + std::to_string(index) + " has DuckDB type " +
									actual.ToString() + "; expected " + expected.ToString());
	}
	if (wire.charset > std::numeric_limits<std::uint8_t>::max()) {
		throw std::invalid_argument("native result charset exceeds the MO type ABI");
	}
}

struct encoded_column {
	std::string data;
	std::string area;
	std::string nulls;
};

encoded_column encode_column(const duckdb::Vector &source, std::size_t row_offset, std::size_t row_count,
							 const native_result_column &schema) {
	duckdb::UnifiedVectorFormat format;
	const_cast<duckdb::Vector &>(source).ToUnifiedFormat(row_offset + row_count, format);
	encoded_column result;
	const auto type_size = static_cast<std::size_t>(mo_type_size(schema.oid));
	result.data.reserve(row_count * type_size);

	std::vector<std::uint64_t> null_words((row_count + 63U) / 64U, 0);
	std::uint64_t null_count = 0;
	for (std::size_t row = 0; row < row_count; ++row) {
		const auto source_row = format.sel->get_index(static_cast<duckdb::idx_t>(row_offset + row));
		const bool valid = format.validity.RowIsValid(source_row);
		if (!valid) {
			null_words[row / 64U] |= std::uint64_t{1} << (row % 64U);
			++null_count;
			if (schema.not_nullable) {
				throw std::invalid_argument("required native result column contains nulls");
			}
		}

		if (is_text(schema.oid)) {
			tae::Varlena output{};
			if (valid) {
				const auto value = reinterpret_cast<const duckdb::string_t *>(format.data)[source_row];
				const auto length = static_cast<std::size_t>(value.GetSize());
				if (length <= tae::VARLENA_INLINE_MAX) {
					output.data[0] = static_cast<std::uint8_t>(length);
					std::memcpy(output.data + 1, value.GetDataUnsafe(), length);
				} else {
					if (result.area.size() > std::numeric_limits<std::uint32_t>::max() ||
						length > std::numeric_limits<std::uint32_t>::max() - result.area.size()) {
						throw std::overflow_error("native result string area exceeds uint32");
					}
					const auto marker = tae::VARLENA_BIG_MARKER;
					const auto offset = static_cast<std::uint32_t>(result.area.size());
					const auto size = static_cast<std::uint32_t>(length);
					std::memcpy(output.data, &marker, sizeof(marker));
					std::memcpy(output.data + 4, &offset, sizeof(offset));
					std::memcpy(output.data + 8, &size, sizeof(size));
					result.area.append(value.GetDataUnsafe(), length);
				}
			}
			result.data.append(reinterpret_cast<const char *>(&output), sizeof(output));
			continue;
		}

		if (!valid) {
			result.data.append(type_size, '\0');
			continue;
		}
		const auto *data = reinterpret_cast<const std::uint8_t *>(format.data);
		if (schema.oid == tae::MO_T_uint32) {
			const auto value = reinterpret_cast<const std::int64_t *>(data)[source_row];
			if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max()) {
				throw std::overflow_error("Substrait i64 result is outside MatrixOne uint32 range");
			}
			append_scalar(result.data, static_cast<std::uint32_t>(value));
		} else if (schema.oid == tae::MO_T_date) {
			const auto value = reinterpret_cast<const std::int32_t *>(data)[source_row];
			if (value > std::numeric_limits<std::int32_t>::max() - tae::MO_UNIX_EPOCH_DAYS) {
				throw std::overflow_error("DuckDB date is outside MatrixOne date range");
			}
			append_scalar(result.data, static_cast<std::int32_t>(value + tae::MO_UNIX_EPOCH_DAYS));
		} else {
			result.data.append(reinterpret_cast<const char *>(data + source_row * type_size), type_size);
		}
	}

	if (null_count != 0) {
		append_scalar(result.nulls, static_cast<std::int64_t>(null_count));
		append_scalar(result.nulls, static_cast<std::uint64_t>(row_count));
		append_scalar(result.nulls, static_cast<std::uint64_t>(null_words.size() * sizeof(std::uint64_t)));
		result.nulls.append(reinterpret_cast<const char *>(null_words.data()),
							null_words.size() * sizeof(std::uint64_t));
	}
	return result;
}

using host_allocation = cucascade::memory::fixed_size_host_memory_resource::multiple_blocks_allocation;

void copy_host_bytes(const std::unique_ptr<host_allocation> &allocation, std::size_t offset, std::size_t size,
					 void *target) {
	if (!allocation || offset > allocation->size_bytes() || size > allocation->size_bytes() - offset) {
		throw std::invalid_argument("native result host buffer range is invalid");
	}
	auto *output = static_cast<std::uint8_t *>(target);
	while (size != 0) {
		const auto block = offset / allocation->block_size();
		const auto in_block = offset % allocation->block_size();
		const auto count = std::min(size, allocation->block_size() - in_block);
		std::memcpy(output, allocation->get_blocks()[block] + in_block, count);
		offset += count;
		output += count;
		size -= count;
	}
}

template <typename T> T read_host_scalar(const std::unique_ptr<host_allocation> &allocation, std::size_t offset) {
	T result{};
	copy_host_bytes(allocation, offset, sizeof(result), &result);
	return result;
}

bool source_type_matches(const cucascade::memory::column_metadata &column, const native_result_column &schema) {
	using id = cudf::type_id;
	switch (schema.oid) {
	case tae::MO_T_bool:
		return column.type_id == id::BOOL8;
	case tae::MO_T_int8:
		return column.type_id == id::INT8;
	case tae::MO_T_int16:
		return column.type_id == id::INT16;
	case tae::MO_T_int32:
		return column.type_id == id::INT32;
	case tae::MO_T_int64:
	case tae::MO_T_uint32:
		return column.type_id == id::INT64;
	case tae::MO_T_float32:
		return column.type_id == id::FLOAT32;
	case tae::MO_T_float64:
		return column.type_id == id::FLOAT64;
	case tae::MO_T_decimal64:
		return ((column.type_id == id::DECIMAL32 || column.type_id == id::DECIMAL64) &&
				column.scale == -schema.scale) ||
			   column.type_id == id::FLOAT64;
	case tae::MO_T_decimal128:
		return (column.type_id == id::DECIMAL128 && column.scale == -schema.scale) || column.type_id == id::FLOAT64 ||
			   (column.type_id == id::INT64 && schema.scale == 0);
	case tae::MO_T_date:
		return column.type_id == id::TIMESTAMP_DAYS;
	case tae::MO_T_char:
	case tae::MO_T_varchar:
		return column.type_id == id::STRING;
	default:
		return false;
	}
}

std::vector<bool> read_nulls(const cucascade::memory::column_metadata &column,
							 const std::unique_ptr<host_allocation> &allocation, std::size_t row_offset,
							 std::size_t row_count, bool required) {
	std::vector<bool> result(row_count, false);
	if (!column.has_null_mask) {
		return result;
	}
	if (column.null_mask_size < (row_offset + row_count + 7U) / 8U) {
		throw std::invalid_argument("Sirius result null mask is shorter than its row range");
	}
	for (std::size_t row = 0; row < row_count; ++row) {
		const auto source_row = row_offset + row;
		const auto byte = read_host_scalar<std::uint8_t>(allocation, column.null_mask_offset + source_row / 8U);
		result[row] = (byte & (std::uint8_t{1} << (source_row % 8U))) == 0;
		if (required && result[row]) {
			throw std::invalid_argument("required native result column contains nulls");
		}
	}
	return result;
}

std::string encode_nulls(const std::vector<bool> &nulls) {
	std::uint64_t count = 0;
	std::vector<std::uint64_t> words((nulls.size() + 63U) / 64U, 0);
	for (std::size_t row = 0; row < nulls.size(); ++row) {
		if (nulls[row]) {
			words[row / 64U] |= std::uint64_t{1} << (row % 64U);
			++count;
		}
	}
	if (count == 0) {
		return {};
	}
	std::string result;
	append_scalar(result, static_cast<std::int64_t>(count));
	append_scalar(result, static_cast<std::uint64_t>(nulls.size()));
	append_scalar(result, static_cast<std::uint64_t>(words.size() * sizeof(std::uint64_t)));
	result.append(reinterpret_cast<const char *>(words.data()), words.size() * sizeof(std::uint64_t));
	return result;
}

encoded_column encode_host_column(const cucascade::memory::column_metadata &column,
								  const std::unique_ptr<host_allocation> &allocation, std::size_t row_offset,
								  std::size_t row_count, const native_result_column &schema) {
	if (!source_type_matches(column, schema) || column.num_rows < 0 ||
		row_offset > static_cast<std::size_t>(column.num_rows) ||
		row_count > static_cast<std::size_t>(column.num_rows) - row_offset) {
		throw std::invalid_argument("Sirius host result column does not match the MO native schema");
	}
	encoded_column result;
	auto nulls = read_nulls(column, allocation, row_offset, row_count, schema.not_nullable);
	result.nulls = encode_nulls(nulls);

	if (is_text(schema.oid)) {
		if (column.children.size() != 1 || !column.has_data || !column.children[0].has_data ||
			(column.children[0].type_id != cudf::type_id::INT32 &&
			 column.children[0].type_id != cudf::type_id::INT64)) {
			throw std::invalid_argument("Sirius string result has invalid offsets or characters");
		}
		const auto offset_width = column.children[0].type_id == cudf::type_id::INT32 ? 4U : 8U;
		if (column.children[0].data_size < (static_cast<std::size_t>(column.num_rows) + 1U) * offset_width) {
			throw std::invalid_argument("Sirius string result offset buffer is truncated");
		}
		auto offset_at = [&](std::size_t row) -> std::uint64_t {
			const auto &offsets = column.children[0];
			return offsets.type_id == cudf::type_id::INT32
					   ? static_cast<std::uint64_t>(read_host_scalar<std::int32_t>(
							 allocation, offsets.data_offset + row * sizeof(std::int32_t)))
					   : static_cast<std::uint64_t>(read_host_scalar<std::int64_t>(
							 allocation, offsets.data_offset + row * sizeof(std::int64_t)));
		};
		result.data.reserve(row_count * tae::VARLENA_SIZE);
		for (std::size_t row = 0; row < row_count; ++row) {
			tae::Varlena value{};
			if (!nulls[row]) {
				const auto begin = offset_at(row_offset + row);
				const auto end = offset_at(row_offset + row + 1);
				if (end < begin || end > column.data_size) {
					throw std::invalid_argument("Sirius string offsets exceed the character buffer");
				}
				const auto length = static_cast<std::size_t>(end - begin);
				if (length <= tae::VARLENA_INLINE_MAX) {
					value.data[0] = static_cast<std::uint8_t>(length);
					copy_host_bytes(allocation, column.data_offset + begin, length, value.data + 1);
				} else {
					if (result.area.size() > std::numeric_limits<std::uint32_t>::max() ||
						length > std::numeric_limits<std::uint32_t>::max() - result.area.size()) {
						throw std::overflow_error("native result string area exceeds uint32");
					}
					const auto marker = tae::VARLENA_BIG_MARKER;
					const auto area_offset = static_cast<std::uint32_t>(result.area.size());
					const auto area_length = static_cast<std::uint32_t>(length);
					std::memcpy(value.data, &marker, 4);
					std::memcpy(value.data + 4, &area_offset, 4);
					std::memcpy(value.data + 8, &area_length, 4);
					const auto old = result.area.size();
					result.area.resize(old + length);
					copy_host_bytes(allocation, column.data_offset + begin, length, result.area.data() + old);
				}
			}
			result.data.append(reinterpret_cast<const char *>(&value), sizeof(value));
		}
		return result;
	}

	const auto target_size = static_cast<std::size_t>(mo_type_size(schema.oid));
	const auto source_size = column.type_id == cudf::type_id::FLOAT64 || schema.oid == tae::MO_T_uint32 ||
									 (schema.oid == tae::MO_T_decimal128 && column.type_id == cudf::type_id::INT64)
								 ? 8U
							 : schema.oid == tae::MO_T_decimal64 && column.type_id == cudf::type_id::DECIMAL32
								 ? 4U
								 : target_size;
	if (!column.has_data || column.data_size < static_cast<std::size_t>(column.num_rows) * source_size) {
		throw std::invalid_argument("Sirius fixed-width result buffer is truncated");
	}
	result.data.reserve(row_count * target_size);
	for (std::size_t row = 0; row < row_count; ++row) {
		if (nulls[row]) {
			result.data.append(target_size, '\0');
			continue;
		}
		const auto source_row = row_offset + row;
		if (schema.oid == tae::MO_T_uint32) {
			const auto value = read_host_scalar<std::int64_t>(allocation, column.data_offset + source_row * 8U);
			if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max()) {
				throw std::overflow_error("Substrait i64 result is outside MatrixOne uint32 range");
			}
			append_scalar(result.data, static_cast<std::uint32_t>(value));
		} else if (schema.oid == tae::MO_T_date) {
			const auto value = read_host_scalar<std::int32_t>(allocation, column.data_offset + source_row * 4U);
			if (value > std::numeric_limits<std::int32_t>::max() - tae::MO_UNIX_EPOCH_DAYS) {
				throw std::overflow_error("Sirius date is outside MatrixOne date range");
			}
			append_scalar(result.data, static_cast<std::int32_t>(value + tae::MO_UNIX_EPOCH_DAYS));
		} else if (schema.oid == tae::MO_T_decimal64 && column.type_id == cudf::type_id::DECIMAL32) {
			append_scalar(result.data, static_cast<std::int64_t>(read_host_scalar<std::int32_t>(
										   allocation, column.data_offset + source_row * 4U)));
		} else if ((schema.oid == tae::MO_T_decimal64 || schema.oid == tae::MO_T_decimal128) &&
				   column.type_id == cudf::type_id::FLOAT64) {
			const auto value = read_host_scalar<double>(allocation, column.data_offset + source_row * 8U);
			if (!std::isfinite(value)) {
				throw std::overflow_error("non-finite Sirius DOUBLE cannot be encoded as MatrixOne DECIMAL");
			}
			const auto rounded =
				std::round(static_cast<long double>(value) * std::pow(10.0L, static_cast<long double>(schema.scale)));
			const auto magnitude_limit = std::pow(10.0L, static_cast<long double>(schema.width));
			if (!std::isfinite(rounded) || rounded <= -magnitude_limit || rounded >= magnitude_limit) {
				throw std::overflow_error("Sirius DOUBLE is outside the MatrixOne DECIMAL range");
			}
			if (schema.oid == tae::MO_T_decimal64) {
				if (rounded < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
					rounded > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
					throw std::overflow_error("Sirius DOUBLE is outside the MatrixOne decimal64 range");
				}
				append_scalar(result.data, static_cast<std::int64_t>(rounded));
			} else {
				const auto decimal = static_cast<__int128_t>(rounded);
				result.data.append(reinterpret_cast<const char *>(&decimal), sizeof(decimal));
			}
		} else if (schema.oid == tae::MO_T_decimal128 && schema.scale == 0 && column.type_id == cudf::type_id::INT64) {
			const auto decimal = static_cast<__int128_t>(
				read_host_scalar<std::int64_t>(allocation, column.data_offset + source_row * 8U));
			result.data.append(reinterpret_cast<const char *>(&decimal), sizeof(decimal));
		} else {
			const auto old = result.data.size();
			result.data.resize(old + target_size);
			copy_host_bytes(allocation, column.data_offset + source_row * target_size, target_size,
							result.data.data() + old);
		}
	}
	return result;
}

std::string assemble_native_batch(std::size_t row_count, const native_result_schema &schema,
								  std::vector<encoded_column> columns) {
	std::string output;
	append_scalar(output, static_cast<std::int64_t>(row_count));
	append_scalar(output, static_cast<std::int32_t>(columns.size()));
	for (std::size_t column = 0; column < columns.size(); ++column) {
		auto &encoded = columns[column];
		std::string vector;
		vector.push_back('\0');
		tae::MOType type{static_cast<std::uint8_t>(schema.columns[column].oid),
						 static_cast<std::uint8_t>(schema.columns[column].charset),
						 static_cast<std::uint8_t>(schema.columns[column].not_nullable ? 1 : 0),
						 0,
						 mo_type_size(schema.columns[column].oid),
						 schema.columns[column].width,
						 schema.columns[column].scale};
		vector.append(reinterpret_cast<const char *>(&type), sizeof(type));
		append_u32(vector, row_count, "native result row count");
		append_u32(vector, encoded.data.size(), "native result vector data");
		vector.append(encoded.data);
		append_u32(vector, encoded.area.size(), "native result vector area");
		vector.append(encoded.area);
		append_u32(vector, encoded.nulls.size(), "native result null bitmap");
		vector.append(encoded.nulls);
		vector.push_back('\0');
		append_u32(output, vector.size(), "native result vector");
		output.append(vector);
	}
	append_scalar(output, std::int32_t{0});
	append_scalar(output, std::int32_t{0});
	append_scalar(output, std::int32_t{0});
	append_scalar(output, std::int32_t{0});
	return output;
}

} // namespace

void validate_native_result_schema(const native_result_schema &wire, const sirius::offload::execution_schema &actual) {
	if (wire.version != k_native_result_schema_version || wire.columns.empty() ||
		wire.columns.size() != actual.types.size() || wire.columns.size() != actual.names.size()) {
		throw std::invalid_argument("native result schema does not match the prepared result width");
	}
	for (std::size_t i = 0; i < wire.columns.size(); ++i) {
		if (wire.columns[i].name != actual.names[i]) {
			throw std::invalid_argument("native result column name does not match the prepared result");
		}
		validate_column(wire.columns[i], actual.types[i], i);
	}
}

std::string encode_native_result_batch(const duckdb::DataChunk &chunk, std::size_t row_offset, std::size_t row_count,
									   const native_result_schema &schema) {
	if (row_count == 0 || row_offset > chunk.size() || row_count > chunk.size() - row_offset ||
		chunk.ColumnCount() != schema.columns.size()) {
		throw std::invalid_argument("invalid native result batch range or width");
	}
	std::vector<std::string> vectors;
	vectors.reserve(schema.columns.size());
	for (std::size_t column = 0; column < schema.columns.size(); ++column) {
		auto encoded = encode_column(chunk.data[column], row_offset, row_count, schema.columns[column]);
		std::string vector;
		vector.reserve(1 + mo_type_bytes + 4 + 4 + encoded.data.size() + 4 + encoded.area.size() + 4 +
					   encoded.nulls.size() + 1);
		vector.push_back('\0'); // vector.VectorClassFlat
		tae::MOType type{static_cast<std::uint8_t>(schema.columns[column].oid),
						 static_cast<std::uint8_t>(schema.columns[column].charset),
						 static_cast<std::uint8_t>(schema.columns[column].not_nullable ? 1 : 0),
						 0,
						 mo_type_size(schema.columns[column].oid),
						 schema.columns[column].width,
						 schema.columns[column].scale};
		vector.append(reinterpret_cast<const char *>(&type), sizeof(type));
		append_u32(vector, row_count, "native result row count");
		append_u32(vector, encoded.data.size(), "native result vector data");
		vector.append(encoded.data);
		append_u32(vector, encoded.area.size(), "native result vector area");
		vector.append(encoded.area);
		append_u32(vector, encoded.nulls.size(), "native result null bitmap");
		vector.append(encoded.nulls);
		vector.push_back('\0'); // not sorted
		vectors.push_back(std::move(vector));
	}

	std::string output;
	append_scalar(output, static_cast<std::int64_t>(row_count));
	append_scalar(output, static_cast<std::int32_t>(vectors.size()));
	for (const auto &vector : vectors) {
		append_u32(output, vector.size(), "native result vector");
		output.append(vector);
	}
	append_scalar(output, std::int32_t{0}); // attrs
	append_scalar(output, std::int32_t{0}); // extra
	append_scalar(output, std::int32_t{0}); // recursive
	append_scalar(output, std::int32_t{0}); // shuffle index
	return output;
}

struct native_result_batch_encoder::impl {
	std::shared_ptr<cucascade::data_batch> source;
	std::shared_ptr<cucascade::data_batch> converted;
	const cucascade::host_data_representation *host = nullptr;
	const cucascade::gpu_table_representation *gpu = nullptr;
	const native_result_schema *schema = nullptr;
	std::vector<sirius::offload::mo_native_result_column> gpu_schema;
	rmm::cuda_stream_view stream;
	std::size_t row_count = 0;
};

native_result_batch_encoder::native_result_batch_encoder(const std::shared_ptr<cucascade::data_batch> &batch,
														 duckdb::ClientContext &context,
														 const native_result_schema &schema,
														 rmm::cuda_stream_view stream)
	: impl_(std::make_unique<impl>()) {
	if (!batch || !batch->get_data()) {
		throw std::invalid_argument("native result encoder received an empty Sirius data batch");
	}
	impl_->source = batch;
	impl_->schema = &schema;
	impl_->stream = stream;
	auto *data = batch->get_data();
	if (data->get_current_tier() == cucascade::memory::Tier::GPU) {
		auto *gpu = dynamic_cast<const cucascade::gpu_table_representation *>(data);
		if (!gpu) {
			throw std::invalid_argument("native result encoder received an unknown GPU representation");
		}
		impl_->gpu_schema.reserve(schema.columns.size());
		for (const auto &column : schema.columns) {
			impl_->gpu_schema.push_back({column.oid, column.width, column.scale, column.charset, column.not_nullable});
		}
		std::size_t threshold = 1U * 1024U * 1024U;
		if (const char *configured = std::getenv("MO_SIDECAR_GPU_RESULT_PACK_MIN_BYTES")) {
			char *end = nullptr;
			errno = 0;
			const auto parsed = std::strtoull(configured, &end, 10);
			if (errno == 0 && end != configured && *end == '\0') {
				threshold = std::clamp<std::size_t>(parsed, 64U * 1024U, 64U * 1024U * 1024U);
			}
		}
		if (sirius::offload::can_pack_mo_native_result_on_gpu(gpu->get_table().view(), impl_->gpu_schema, threshold)) {
			impl_->gpu = gpu;
			impl_->row_count = static_cast<std::size_t>(gpu->get_table().num_rows());
			return;
		}
		auto sirius_ctx = context.registered_state->Get<duckdb::SiriusContext>("sirius_state");
		if (!sirius_ctx) {
			throw std::runtime_error("native result encoder requires an initialized Sirius context");
		}
		auto reservation = sirius_ctx->get_memory_manager().request_reservation(
			cucascade::memory::any_memory_space_in_tier{cucascade::memory::Tier::HOST}, data->get_size_in_bytes());
		if (!reservation) {
			throw std::runtime_error("native result encoder failed to reserve host memory");
		}
		auto &repo = sirius_ctx->get_data_repository_manager();
		impl_->converted = batch->clone(repo.get_next_data_batch_id(), stream);
		impl_->converted->convert_to<cucascade::host_data_representation>(sirius::converter_registry::get(),
																		  &reservation->get_memory_space(), stream);
		data = impl_->converted->get_data();
	}
	if (data->get_current_tier() != cucascade::memory::Tier::HOST) {
		throw std::invalid_argument("native result encoder only accepts Sirius GPU or host data");
	}
	impl_->host = dynamic_cast<const cucascade::host_data_representation *>(data);
	if (!impl_->host || !impl_->host->get_host_table() || !impl_->host->get_host_table()->allocation ||
		impl_->host->get_host_table()->columns.size() != schema.columns.size()) {
		throw std::invalid_argument("native result encoder received invalid Sirius host storage");
	}
	const auto &columns = impl_->host->get_host_table()->columns;
	impl_->row_count = columns.empty() ? 0 : static_cast<std::size_t>(columns[0].num_rows);
	for (std::size_t i = 0; i < columns.size(); ++i) {
		const auto &column = columns[i];
		if (column.num_rows < 0 || static_cast<std::size_t>(column.num_rows) != impl_->row_count) {
			throw std::invalid_argument("native result columns have inconsistent row counts");
		}
		if (!source_type_matches(column, schema.columns[i])) {
			throw std::invalid_argument("Sirius result column does not match the MO native schema");
		}
	}
}

native_result_batch_encoder::~native_result_batch_encoder() = default;
native_result_batch_encoder::native_result_batch_encoder(native_result_batch_encoder &&) noexcept = default;
native_result_batch_encoder &native_result_batch_encoder::operator=(native_result_batch_encoder &&) noexcept = default;

std::size_t native_result_batch_encoder::rows() const noexcept { return impl_ ? impl_->row_count : 0; }

std::size_t native_result_batch_encoder::encoded_size(std::size_t row_offset, std::size_t row_count) const {
	if (!impl_ || row_count == 0 || row_offset > impl_->row_count || row_count > impl_->row_count - row_offset) {
		throw std::invalid_argument("invalid native result encoder row range");
	}
	std::size_t total = 12U + 16U; // row count, width, and four terminal int32 fields
	auto add = [&](std::size_t value) {
		if (value > std::numeric_limits<std::size_t>::max() - total) {
			throw std::overflow_error("native result encoded size overflows size_t");
		}
		total += value;
	};
	if (impl_->gpu) {
		for (const auto &column : impl_->schema->columns) {
			add(4U + 34U + row_count * static_cast<std::size_t>(mo_type_size(column.oid)));
		}
		return total;
	}

	const auto &table = *impl_->host->get_host_table();
	for (std::size_t i = 0; i < table.columns.size(); ++i) {
		const auto &column = table.columns[i];
		const auto &schema = impl_->schema->columns[i];
		std::size_t area_bytes = 0;
		if (is_text(schema.oid)) {
			if (column.children.size() != 1 || !column.children[0].has_data ||
				(column.children[0].type_id != cudf::type_id::INT32 &&
				 column.children[0].type_id != cudf::type_id::INT64)) {
				throw std::invalid_argument("Sirius string result has invalid offsets");
			}
			const auto &offsets = column.children[0];
			const auto offset_width = offsets.type_id == cudf::type_id::INT32 ? 4U : 8U;
			if (offsets.data_size < (static_cast<std::size_t>(column.num_rows) + 1U) * offset_width) {
				throw std::invalid_argument("Sirius string result offset buffer is truncated");
			}
			auto offset_at = [&](std::size_t row) -> std::uint64_t {
				return offsets.type_id == cudf::type_id::INT32
						   ? static_cast<std::uint64_t>(
								 read_host_scalar<std::int32_t>(table.allocation, offsets.data_offset + row * 4U))
						   : static_cast<std::uint64_t>(
								 read_host_scalar<std::int64_t>(table.allocation, offsets.data_offset + row * 8U));
			};
			for (std::size_t row = 0; row < row_count; ++row) {
				const auto begin = offset_at(row_offset + row);
				const auto end = offset_at(row_offset + row + 1U);
				if (end < begin || end > column.data_size) {
					throw std::invalid_argument("Sirius string offsets exceed the character buffer");
				}
				if (end - begin > tae::VARLENA_INLINE_MAX) {
					if (end - begin > std::numeric_limits<std::size_t>::max() - area_bytes) {
						throw std::overflow_error("native result string area overflows size_t");
					}
					area_bytes += static_cast<std::size_t>(end - begin);
				}
			}
		}
		bool has_null = false;
		if (column.has_null_mask) {
			if (column.null_mask_size < (row_offset + row_count + 7U) / 8U) {
				throw std::invalid_argument("Sirius result null mask is shorter than its row range");
			}
			for (std::size_t row = 0; row < row_count && !has_null; ++row) {
				const auto source_row = row_offset + row;
				const auto byte =
					read_host_scalar<std::uint8_t>(table.allocation, column.null_mask_offset + source_row / 8U);
				has_null = (byte & (std::uint8_t{1} << (source_row % 8U))) == 0;
			}
		}
		const auto null_bytes = has_null ? 24U + ((row_count + 63U) / 64U) * 8U : 0U;
		add(4U + 34U + row_count * static_cast<std::size_t>(mo_type_size(schema.oid)) + area_bytes + null_bytes);
	}
	return total;
}

std::string native_result_batch_encoder::encode(std::size_t row_offset, std::size_t row_count) const {
	if (!impl_ || row_count == 0 || row_offset > impl_->row_count || row_count > impl_->row_count - row_offset) {
		throw std::invalid_argument("invalid native result encoder row range");
	}
	if (impl_->gpu) {
		return sirius::offload::pack_mo_native_result_on_gpu(impl_->gpu->get_table().view(), impl_->gpu_schema,
															 row_offset, row_count, impl_->stream);
	}
	const auto &table = *impl_->host->get_host_table();
	std::vector<encoded_column> columns;
	columns.reserve(table.columns.size());
	for (std::size_t i = 0; i < table.columns.size(); ++i) {
		columns.push_back(
			encode_host_column(table.columns[i], table.allocation, row_offset, row_count, impl_->schema->columns[i]));
	}
	return assemble_native_batch(row_count, *impl_->schema, std::move(columns));
}

} // namespace matrixone::sidecar
