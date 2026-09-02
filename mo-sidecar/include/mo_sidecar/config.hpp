// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace matrixone::sidecar {

struct https_endpoint {
	std::string host;
	int port = 443;
	std::string path;
};

struct runtime_config {
	std::string flight_host = "0.0.0.0";
	int flight_port = 0;
	std::string flight_cert_path;
	std::string flight_key_path;
	std::string flight_client_ca_path;
	https_endpoint read_endpoint;
	std::string read_ca_path;
	std::string read_client_cert_path;
	std::string read_client_key_path;
	std::size_t max_active_tickets = 128;
	std::uint64_t max_batch_bytes = 64U * 1024U * 1024U;
	std::uint64_t stream_input_capacity_bytes = 2ULL * 1024U * 1024U * 1024U;
	std::uint64_t fatal_shutdown_grace_ms = 5000;
	std::uint64_t ticket_ttl_ms = 15U * 60U * 1000U;
};

std::optional<runtime_config> load_runtime_config();
https_endpoint parse_https_endpoint(const std::string &url);
std::string read_secret_file(const std::string &path, std::size_t max_bytes = 1024U * 1024U);

} // namespace matrixone::sidecar
