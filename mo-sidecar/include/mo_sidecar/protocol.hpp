// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sirius::offload {
struct stream_read;
struct tae_read;
} // namespace sirius::offload

namespace matrixone::sidecar {

inline constexpr std::uint32_t k_protocol_version = 5;
inline constexpr std::uint32_t k_tae_read_protocol_version = 2;
inline constexpr std::uint32_t k_stream_read_protocol_version = 1;
inline constexpr std::string_view k_substrait_version = "0.78.0";
inline constexpr std::size_t k_sha256_bytes = 32;
inline constexpr std::size_t k_max_plan_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t k_max_stream_inputs = 16U;
inline constexpr std::uint64_t k_max_stream_input_batch_bytes = 4U * 1024U * 1024U;
inline constexpr std::uint32_t k_native_batch_codec_version = 1U;
inline constexpr std::uint32_t k_native_result_schema_version = 1U;
inline constexpr std::size_t k_native_batch_frame_header_bytes = 24U;
inline constexpr std::size_t k_max_native_result_schema_bytes = 1U * 1024U * 1024U;
inline constexpr std::size_t k_max_native_result_columns = 4096U;

struct native_result_column {
	std::string name;
	std::uint32_t oid = 0;
	std::int32_t width = 0;
	std::int32_t scale = 0;
	std::uint32_t charset = 0;
	bool not_nullable = false;
};

struct native_result_schema {
	std::uint32_t version = 0;
	std::vector<native_result_column> columns;
};

struct execute_request {
	std::uint32_t protocol_version = 0;
	std::string substrait_version;
	std::string capability_hash;
	std::uint64_t max_batch_bytes = 0;
	std::uint64_t deadline_unix_ms = 0;
	std::string plan;
	std::string query_id;
	std::string idempotency_key;
	std::uint64_t account_id = 0;
	std::uint64_t max_input_batch_bytes = 0;
	std::string result_schema;
	std::string fingerprint;
};

struct upload_input_request {
	std::string ticket;
	std::string stream_ref;
};

struct upload_input_ack {
	std::uint64_t acknowledged_batches = 0;
	std::uint64_t rows = 0;
	std::uint64_t bytes = 0;
	bool complete = false;
	bool not_needed = false;
	bool ready = false;
};

struct native_batch_frame {
	std::uint64_t sequence = 0;
	std::string_view payload;
};

struct cancel_request {
	std::string ticket;
	std::string idempotency_key;
};

struct resolve_response {
	std::string tae_read;
	std::string manifest;
	std::string canonical_schema;
};

execute_request parse_execute_request(std::string_view bytes);
cancel_request parse_cancel_request(std::string_view bytes);
upload_input_request parse_upload_input_request(std::string_view bytes);
native_batch_frame parse_native_batch_frame(std::string_view bytes);
native_result_schema parse_native_result_schema(std::string_view bytes);
resolve_response parse_resolve_response(std::string_view bytes);
std::string serialize_upload_input_ack(const upload_input_ack &ack);
std::string serialize_native_batch_frame(std::uint64_t sequence, std::string_view payload);
std::string serialize_resolve_request(const sirius::offload::tae_read &request, std::string_view requested_schema);
std::string serialize_tae_read(const sirius::offload::tae_read &request);
std::string serialize_stream_read(const sirius::offload::stream_read &request);

std::array<unsigned char, k_sha256_bytes> sha256(std::string_view bytes);
std::string sha256_bytes(std::string_view bytes);
std::string hex(std::string_view bytes);
std::string execution_idempotency_key(std::uint64_t account_id, std::string_view query_id);

std::string_view capability_document();
const std::string &capability_hash();
std::uint64_t now_unix_ms();

} // namespace matrixone::sidecar
