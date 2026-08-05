// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace sirius::offload {
struct tae_read;
}

namespace matrixone::sidecar {

inline constexpr std::uint32_t k_protocol_version = 1;
inline constexpr std::string_view k_substrait_version = "0.78.0";
inline constexpr std::size_t k_sha256_bytes = 32;
inline constexpr std::size_t k_max_plan_bytes = 16U * 1024U * 1024U;

struct execute_request {
	std::uint32_t protocol_version = 0;
	std::string substrait_version;
	std::string capability_hash;
	std::uint64_t max_batch_bytes = 0;
	std::uint64_t deadline_unix_ms = 0;
	std::string plan;
};

struct resolve_response {
	std::string tae_read;
	std::string manifest;
	std::string canonical_schema;
};

execute_request parse_execute_request(std::string_view bytes);
resolve_response parse_resolve_response(std::string_view bytes);
std::string serialize_resolve_request(const sirius::offload::tae_read &request, std::string_view requested_schema);
std::string serialize_tae_read(const sirius::offload::tae_read &request);

std::array<unsigned char, k_sha256_bytes> sha256(std::string_view bytes);
std::string sha256_bytes(std::string_view bytes);
std::string hex(std::string_view bytes);

std::string_view capability_document();
const std::string &capability_hash();
std::uint64_t now_unix_ms();

} // namespace matrixone::sidecar
