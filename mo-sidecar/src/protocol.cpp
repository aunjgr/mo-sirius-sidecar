// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/protocol.hpp"

#include "offload/stream_read.hpp"
#include "offload/tae_read.hpp"

#include <openssl/evp.h>

#include <chrono>
#include <limits>
#include <stdexcept>

namespace matrixone::sidecar {
namespace {

constexpr std::string_view k_capability_document =
	"{\"protocol_version\":5,\"substrait_version\":\"0.78.0\","
	"\"tae_read_protocol_version\":2,\"tae_read_feature_bits\":0,"
	"\"stream_read_protocol_version\":1,\"stream_read_feature_bits\":0,"
	"\"operators\":[\"read\",\"filter\",\"project\",\"aggregate\",\"sort\","
	"\"fetch\",\"join\",\"reference\"],"
	"\"join_types\":[\"inner\",\"left\",\"right\",\"left_semi\",\"left_anti\","
	"\"right_semi\",\"right_anti\"],"
	"\"expressions\":[\"literal\",\"selection\",\"scalar_function\",\"cast\","
	"\"if_then\",\"singular_or_list\"],"
	"\"types\":[\"bool\",\"i8\",\"i16\",\"i32\",\"i64\",\"fp32\",\"fp64\","
	"\"varchar\",\"decimal\",\"date\"],"
	"\"semantic_registry\":\"exact-mo-bound-overload-and-tpch-family-v1\","
	"\"scalar_functions\":[\"and\",\"or\",\"not\",\"equal\",\"not_equal\","
	"\"lt\","
	"\"lte\",\"gt\",\"gte\",\"is_null\",\"is_not_null\",\"is_not_distinct_"
	"from\","
	"\"add\",\"subtract\",\"multiply\",\"divide\",\"modulus\",\"between\","
	"\"like\","
	"\"starts_with\",\"substring\",\"extract\"],"
	"\"aggregate_functions\":[\"count\",\"sum\",\"min\",\"max\",\"avg\"],"
	"\"transport\":\"arrow-flight\",\"stream_input_transport\":\"flight-do-put-"
	"mo-native\","
	"\"result_transport\":\"flight-doget-mo-native\","
	"\"mo_native_result_schema_version\":1,"
	"\"gpu_mo_batch_ingress\":true,\"adaptive_gpu_result_pack\":true,"
	"\"mo_native_batch_codec_version\":1,\"mo_type_size\":16,\"mo_varlena_"
	"size\":24,"
	"\"mo_native_endian\":\"little\","
	"\"stream_input_ack\":\"ready-consumed-final-v1\","
	"\"stream_input_slots_per_read\":1,"
	"\"max_stream_inputs\":16,\"max_stream_input_batch_bytes\":4194304,"
	"\"sirius_execution_contract\":1,"
	"\"max_plan_bytes\":16777216}";

class wire_reader {
  public:
	explicit wire_reader(std::string_view input) : input_(input) {}

	bool done() const noexcept { return position_ == input_.size(); }

	std::uint64_t varint() {
		std::uint64_t value = 0;
		for (unsigned shift = 0; shift < 70; shift += 7) {
			if (position_ == input_.size()) {
				throw std::invalid_argument("truncated protobuf varint");
			}
			const auto byte = static_cast<unsigned char>(input_[position_++]);
			if (shift == 63 && byte > 1) {
				throw std::invalid_argument("overflowing protobuf varint");
			}
			value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
			if ((byte & 0x80U) == 0) {
				return value;
			}
		}
		throw std::invalid_argument("overflowing protobuf varint");
	}

	std::string bytes(std::size_t maximum) {
		const auto length = varint();
		if (length > maximum || length > input_.size() - position_) {
			throw std::invalid_argument("invalid protobuf bytes length");
		}
		std::string result(input_.substr(position_, static_cast<std::size_t>(length)));
		position_ += static_cast<std::size_t>(length);
		return result;
	}

  private:
	std::string_view input_;
	std::size_t position_ = 0;
};

void append_varint(std::string &output, std::uint64_t value) {
	while (value >= 0x80U) {
		output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
		value >>= 7U;
	}
	output.push_back(static_cast<char>(value));
}

void append_uint(std::string &output, unsigned field, std::uint64_t value) {
	if (value == 0) {
		return;
	}
	append_varint(output, static_cast<std::uint64_t>(field) << 3U);
	append_varint(output, value);
}

void append_required_uint(std::string &output, unsigned field, std::uint64_t value) {
	append_varint(output, static_cast<std::uint64_t>(field) << 3U);
	append_varint(output, value);
}

void append_bytes(std::string &output, unsigned field, std::string_view value) {
	if (value.empty()) {
		return;
	}
	append_varint(output, (static_cast<std::uint64_t>(field) << 3U) | 2U);
	append_varint(output, value.size());
	output.append(value);
}

void expect_field(std::uint64_t tag, unsigned maximum, std::uint64_t &seen) {
	const auto field = static_cast<unsigned>(tag >> 3U);
	if (field == 0 || field > maximum) {
		throw std::invalid_argument("unknown protobuf field");
	}
	const auto mask = std::uint64_t{1} << (field - 1U);
	if ((seen & mask) != 0) {
		throw std::invalid_argument("duplicate protobuf field");
	}
	seen |= mask;
}

} // namespace

execute_request parse_execute_request(std::string_view bytes) {
	if (bytes.empty() || bytes.size() > k_max_plan_bytes + k_max_native_result_schema_bytes + 4096U) {
		throw std::invalid_argument("ExecuteSubstrait request is empty or too large");
	}
	wire_reader reader(bytes);
	execute_request result;
	std::uint64_t seen = 0;
	while (!reader.done()) {
		const auto tag = reader.varint();
		expect_field(tag, 11, seen);
		const auto field = static_cast<unsigned>(tag >> 3U);
		const auto wire = static_cast<unsigned>(tag & 7U);
		const bool integer = field == 1 || field == 4 || field == 5 || field == 9 || field == 10;
		if (wire != (integer ? 0U : 2U)) {
			throw std::invalid_argument("wrong protobuf wire type");
		}
		switch (field) {
		case 1: {
			const auto value = reader.varint();
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				throw std::invalid_argument("protocol version overflows uint32");
			}
			result.protocol_version = static_cast<std::uint32_t>(value);
			break;
		}
		case 2:
			result.substrait_version = reader.bytes(64);
			break;
		case 3:
			result.capability_hash = reader.bytes(k_sha256_bytes);
			break;
		case 4:
			result.max_batch_bytes = reader.varint();
			break;
		case 5:
			result.deadline_unix_ms = reader.varint();
			break;
		case 6:
			result.plan = reader.bytes(k_max_plan_bytes);
			break;
		case 7:
			result.query_id = reader.bytes(16U);
			break;
		case 8:
			result.idempotency_key = reader.bytes(k_sha256_bytes);
			break;
		case 9:
			result.account_id = reader.varint();
			break;
		case 10:
			result.max_input_batch_bytes = reader.varint();
			break;
		case 11:
			result.result_schema = reader.bytes(k_max_native_result_schema_bytes);
			break;
		default:
			throw std::invalid_argument("unknown ExecuteSubstrait field");
		}
	}
	if (seen != 0x7ffU || result.query_id.size() != 16U || result.idempotency_key.size() != k_sha256_bytes) {
		throw std::invalid_argument("ExecuteSubstrait request is missing a field");
	}
	return result;
}

native_result_schema parse_native_result_schema(std::string_view bytes) {
	if (bytes.empty() || bytes.size() > k_max_native_result_schema_bytes) {
		throw std::invalid_argument("native result schema is empty or too large");
	}
	auto parse_column = [](std::string_view wire_bytes) {
		wire_reader reader(wire_bytes);
		native_result_column result;
		std::uint64_t seen = 0;
		while (!reader.done()) {
			const auto tag = reader.varint();
			expect_field(tag, 6, seen);
			const auto field = static_cast<unsigned>(tag >> 3U);
			const auto wire = static_cast<unsigned>(tag & 7U);
			if (wire != (field == 1 ? 2U : 0U)) {
				throw std::invalid_argument("native result column has the wrong wire type");
			}
			switch (field) {
			case 1:
				result.name = reader.bytes(k_max_native_result_schema_bytes);
				break;
			case 2: {
				const auto value = reader.varint();
				if (value > std::numeric_limits<std::uint32_t>::max()) {
					throw std::invalid_argument("native result OID overflows uint32");
				}
				result.oid = static_cast<std::uint32_t>(value);
				break;
			}
			case 3:
			case 4: {
				const auto value = reader.varint();
				constexpr auto first_sign_extended_int32 =
					std::numeric_limits<std::uint64_t>::max() -
					static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
				if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) &&
					value < first_sign_extended_int32) {
					throw std::invalid_argument("native result width or scale overflows int32");
				}
				const auto signed_value = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
				if (field == 3) {
					result.width = signed_value;
				} else {
					result.scale = signed_value;
				}
				break;
			}
			case 5: {
				const auto value = reader.varint();
				if (value > std::numeric_limits<std::uint8_t>::max()) {
					throw std::invalid_argument("native result charset overflows uint8");
				}
				result.charset = static_cast<std::uint32_t>(value);
				break;
			}
			case 6: {
				const auto value = reader.varint();
				if (value > 1) {
					throw std::invalid_argument("native result nullability is invalid");
				}
				result.not_nullable = value != 0;
				break;
			}
			default:
				throw std::invalid_argument("unknown native result column field");
			}
		}
		if ((seen & (std::uint64_t{1} << 1U)) == 0 || result.oid == 0) {
			throw std::invalid_argument("native result column is missing its OID");
		}
		return result;
	};

	wire_reader reader(bytes);
	native_result_schema result;
	bool seen_version = false;
	while (!reader.done()) {
		const auto tag = reader.varint();
		const auto field = static_cast<unsigned>(tag >> 3U);
		const auto wire = static_cast<unsigned>(tag & 7U);
		if (field == 1) {
			if (seen_version || wire != 0U) {
				throw std::invalid_argument("native result schema version is duplicated or malformed");
			}
			const auto value = reader.varint();
			if (value > std::numeric_limits<std::uint32_t>::max()) {
				throw std::invalid_argument("native result schema version overflows uint32");
			}
			result.version = static_cast<std::uint32_t>(value);
			seen_version = true;
		} else if (field == 2) {
			if (wire != 2U || result.columns.size() >= k_max_native_result_columns) {
				throw std::invalid_argument("native result schema columns are malformed or excessive");
			}
			result.columns.push_back(parse_column(reader.bytes(k_max_native_result_schema_bytes)));
		} else {
			throw std::invalid_argument("unknown native result schema field");
		}
	}
	if (!seen_version || result.version != k_native_result_schema_version || result.columns.empty()) {
		throw std::invalid_argument("native result schema is incomplete or unsupported");
	}
	return result;
}

upload_input_request parse_upload_input_request(std::string_view bytes) {
	if (bytes.empty() || bytes.size() > 2U * (k_sha256_bytes + 2U)) {
		throw std::invalid_argument("UploadInput request is empty or too large");
	}
	wire_reader reader(bytes);
	upload_input_request result;
	std::uint64_t seen = 0;
	while (!reader.done()) {
		const auto tag = reader.varint();
		expect_field(tag, 2, seen);
		if ((tag & 7U) != 2U) {
			throw std::invalid_argument("wrong UploadInput wire type");
		}
		switch (tag >> 3U) {
		case 1:
			result.ticket = reader.bytes(k_sha256_bytes);
			break;
		case 2:
			result.stream_ref = reader.bytes(k_sha256_bytes);
			break;
		default:
			throw std::invalid_argument("unknown UploadInput field");
		}
	}
	if (seen != 0x3U || result.ticket.size() != k_sha256_bytes || result.stream_ref.size() != k_sha256_bytes) {
		throw std::invalid_argument("UploadInput requires a ticket and stream reference");
	}
	return result;
}

std::string serialize_upload_input_ack(const upload_input_ack &ack) {
	std::string output;
	append_uint(output, 1, ack.acknowledged_batches);
	append_uint(output, 2, ack.rows);
	append_uint(output, 3, ack.bytes);
	if (ack.complete) {
		append_uint(output, 4, 1);
	}
	if (ack.not_needed) {
		append_uint(output, 5, 1);
	}
	if (ack.ready) {
		append_uint(output, 6, 1);
	}
	return output;
}

native_batch_frame parse_native_batch_frame(std::string_view bytes) {
	static constexpr std::string_view magic = "MOB1";
	if (bytes.size() < k_native_batch_frame_header_bytes || bytes.substr(0, magic.size()) != magic) {
		throw std::invalid_argument("invalid MO native batch frame magic or length");
	}
	auto read_u16 = [&](std::size_t offset) {
		return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
			   static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U;
	};
	auto read_u64 = [&](std::size_t offset) {
		std::uint64_t value = 0;
		for (unsigned i = 0; i < 8; ++i) {
			value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8U);
		}
		return value;
	};
	if (read_u16(4) != k_native_batch_codec_version || read_u16(6) != 0) {
		throw std::invalid_argument("unsupported MO native batch frame version or flags");
	}
	const auto sequence = read_u64(8);
	const auto payload_size = read_u64(16);
	if (sequence == 0 || payload_size == 0 || payload_size > k_max_stream_input_batch_bytes ||
		payload_size != bytes.size() - k_native_batch_frame_header_bytes) {
		throw std::invalid_argument("invalid MO native batch frame sequence or payload length");
	}
	return native_batch_frame{sequence, bytes.substr(k_native_batch_frame_header_bytes)};
}

std::string serialize_native_batch_frame(std::uint64_t sequence, std::string_view payload) {
	if (sequence == 0 || payload.empty()) {
		throw std::invalid_argument("MO native batch frame requires a sequence and payload");
	}
	std::string result(k_native_batch_frame_header_bytes, '\0');
	result.replace(0, 4, "MOB1");
	result[4] = static_cast<char>(k_native_batch_codec_version & 0xffU);
	result[5] = static_cast<char>((k_native_batch_codec_version >> 8U) & 0xffU);
	auto write_u64 = [&](std::size_t offset, std::uint64_t value) {
		for (unsigned i = 0; i < 8; ++i) {
			result[offset + i] = static_cast<char>((value >> (i * 8U)) & 0xffU);
		}
	};
	write_u64(8, sequence);
	write_u64(16, payload.size());
	result.append(payload);
	return result;
}

cancel_request parse_cancel_request(std::string_view bytes) {
	if (bytes.empty() || bytes.size() > k_sha256_bytes + 2U) {
		throw std::invalid_argument("CancelExecution request is empty or too large");
	}
	wire_reader reader(bytes);
	cancel_request result;
	std::uint64_t seen = 0;
	while (!reader.done()) {
		const auto tag = reader.varint();
		expect_field(tag, 2, seen);
		if ((tag & 7U) != 2U) {
			throw std::invalid_argument("wrong CancelExecution wire type");
		}
		switch (tag >> 3U) {
		case 1:
			result.ticket = reader.bytes(k_sha256_bytes);
			break;
		case 2:
			result.idempotency_key = reader.bytes(k_sha256_bytes);
			break;
		default:
			throw std::invalid_argument("unknown CancelExecution field");
		}
	}
	if ((seen != 0x1U && seen != 0x2U) || (!result.ticket.empty() && result.ticket.size() != k_sha256_bytes) ||
		(!result.idempotency_key.empty() && result.idempotency_key.size() != k_sha256_bytes)) {
		throw std::invalid_argument("CancelExecution requires exactly one 32-byte identity");
	}
	return result;
}

resolve_response parse_resolve_response(std::string_view bytes) {
	constexpr std::size_t max_response_bytes = 64U * 1024U * 1024U;
	if (bytes.empty() || bytes.size() > max_response_bytes) {
		throw std::invalid_argument("ResolveTaeRead response is empty or too large");
	}
	wire_reader reader(bytes);
	resolve_response result;
	std::uint64_t seen = 0;
	while (!reader.done()) {
		const auto tag = reader.varint();
		expect_field(tag, 3, seen);
		if ((tag & 7U) != 2U) {
			throw std::invalid_argument("wrong ResolveTaeRead wire type");
		}
		switch (tag >> 3U) {
		case 1:
			result.tae_read = reader.bytes(16U * 1024U);
			break;
		case 2:
			result.manifest = reader.bytes(max_response_bytes);
			break;
		case 3:
			result.canonical_schema = reader.bytes(1024U * 1024U);
			break;
		default:
			throw std::invalid_argument("unknown ResolveTaeRead field");
		}
	}
	if (seen != 0x7U || result.tae_read.empty() || result.manifest.empty() || result.canonical_schema.empty()) {
		throw std::invalid_argument("ResolveTaeRead response is missing a field");
	}
	return result;
}

std::string serialize_tae_read(const sirius::offload::tae_read &request) {
	std::string output;
	output.reserve(256 + request.read_ref.size() + request.query_id.size());
	append_uint(output, 1, request.protocol_version);
	append_uint(output, 2, request.feature_bits);
	append_bytes(output, 3, request.read_ref);
	append_bytes(output, 4, request.query_id);
	append_required_uint(output, 5, request.account_id);
	append_uint(output, 6, request.table_id);
	append_bytes(output, 7, request.snapshot_ts);
	append_bytes(output, 8, request.schema_digest);
	append_bytes(output, 9, request.manifest_sha256);
	append_bytes(output, 10, request.capability_hash);
	append_uint(output, 11, request.expires_at_unix_ms);
	append_uint(output, 12, request.database_id);
	return output;
}

std::string serialize_stream_read(const sirius::offload::stream_read &request) {
	std::string output;
	output.reserve(256 + request.stream_ref.size() + request.query_id.size());
	append_uint(output, 1, request.protocol_version);
	append_uint(output, 2, request.feature_bits);
	append_bytes(output, 3, request.stream_ref);
	append_bytes(output, 4, request.query_id);
	append_required_uint(output, 5, request.account_id);
	append_bytes(output, 6, request.snapshot_ts);
	append_bytes(output, 7, request.schema_digest);
	append_bytes(output, 8, request.capability_hash);
	append_uint(output, 9, request.expires_at_unix_ms);
	return output;
}

std::string serialize_resolve_request(const sirius::offload::tae_read &request, std::string_view requested_schema) {
	std::string output;
	append_bytes(output, 1, serialize_tae_read(request));
	append_bytes(output, 2, requested_schema);
	return output;
}

std::array<unsigned char, k_sha256_bytes> sha256(std::string_view bytes) {
	std::array<unsigned char, k_sha256_bytes> digest{};
	unsigned int length = 0;
	auto *context = EVP_MD_CTX_new();
	if (!context) {
		throw std::runtime_error("cannot allocate SHA-256 context");
	}
	const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
					EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
					EVP_DigestFinal_ex(context, digest.data(), &length) == 1;
	EVP_MD_CTX_free(context);
	if (!ok || length != digest.size()) {
		throw std::runtime_error("SHA-256 failed");
	}
	return digest;
}

std::string sha256_bytes(std::string_view bytes) {
	const auto digest = sha256(bytes);
	return std::string(reinterpret_cast<const char *>(digest.data()), digest.size());
}

std::string hex(std::string_view bytes) {
	static constexpr char digits[] = "0123456789abcdef";
	std::string result(bytes.size() * 2, '\0');
	for (std::size_t i = 0; i < bytes.size(); ++i) {
		const auto value = static_cast<unsigned char>(bytes[i]);
		result[i * 2] = digits[value >> 4U];
		result[i * 2 + 1] = digits[value & 0xfU];
	}
	return result;
}

std::string execution_idempotency_key(std::uint64_t account_id, std::string_view query_id) {
	if (query_id.size() != 16U) {
		throw std::invalid_argument("invalid execution identity");
	}
	std::string input(8, '\0');
	for (unsigned i = 0; i < 8; ++i) {
		input[i] = static_cast<char>((account_id >> (i * 8U)) & 0xffU);
	}
	input.append(query_id);
	return sha256_bytes(input);
}

std::string_view capability_document() { return k_capability_document; }

const std::string &capability_hash() {
	static const std::string value = sha256_bytes(k_capability_document);
	return value;
}

std::uint64_t now_unix_ms() {
	const auto now = std::chrono::system_clock::now().time_since_epoch();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace matrixone::sidecar
