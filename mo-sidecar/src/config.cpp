// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace matrixone::sidecar {
namespace {

std::string required_env(const char *name) {
	const char *value = std::getenv(name);
	if (!value || *value == '\0') {
		throw std::invalid_argument(std::string(name) + " is required");
	}
	return value;
}

template <typename T>
T parse_integer(const char *name, std::string_view value, T minimum, T maximum) {
	T parsed {};
	const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
	if (result.ec != std::errc() || result.ptr != value.data() + value.size() || parsed < minimum || parsed > maximum) {
		throw std::invalid_argument(std::string(name) + " is outside the accepted range");
	}
	return parsed;
}

template <typename T>
T optional_integer(const char *name, T default_value, T minimum, T maximum) {
	const char *value = std::getenv(name);
	return value && *value ? parse_integer<T>(name, value, minimum, maximum) : default_value;
}

} // namespace

https_endpoint parse_https_endpoint(const std::string &url) {
	constexpr std::string_view prefix = "https://";
	if (!url.starts_with(prefix)) {
		throw std::invalid_argument("MO_SIDECAR_READ_URL must use https://");
	}
	const std::string_view remainder(url.data() + prefix.size(), url.size() - prefix.size());
	const auto slash = remainder.find('/');
	const auto authority = remainder.substr(0, slash);
	const auto path = slash == std::string_view::npos ? std::string_view("/") : remainder.substr(slash);
	if (authority.empty() || path.empty() || path.front() != '/' || path.find('#') != std::string_view::npos) {
		throw std::invalid_argument("MO_SIDECAR_READ_URL is malformed");
	}

	https_endpoint result;
	result.path = std::string(path);
	if (authority.front() == '[') {
		const auto close = authority.find(']');
		if (close == std::string_view::npos) {
			throw std::invalid_argument("MO_SIDECAR_READ_URL has a malformed IPv6 host");
		}
		result.host = std::string(authority.substr(1, close - 1));
		if (close + 1 < authority.size()) {
			if (authority[close + 1] != ':') {
				throw std::invalid_argument("MO_SIDECAR_READ_URL has a malformed authority");
			}
			result.port = parse_integer<int>("MO_SIDECAR_READ_URL port", authority.substr(close + 2), 1, 65535);
		}
	} else {
		const auto colon = authority.rfind(':');
		if (colon == std::string_view::npos) {
			result.host = std::string(authority);
		} else {
			result.host = std::string(authority.substr(0, colon));
			result.port = parse_integer<int>("MO_SIDECAR_READ_URL port", authority.substr(colon + 1), 1, 65535);
		}
	}
	if (result.host.empty() || result.host.find('@') != std::string::npos ||
	    result.host.find('/') != std::string::npos) {
		throw std::invalid_argument("MO_SIDECAR_READ_URL host is malformed");
	}
	return result;
}

std::string read_secret_file(const std::string &path, std::size_t max_bytes) {
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) {
		throw std::runtime_error("cannot open credential file: " + path);
	}
	const auto end = input.tellg();
	if (end < 0 || static_cast<std::uint64_t>(end) > max_bytes) {
		throw std::runtime_error("credential file exceeds size limit: " + path);
	}
	std::string contents(static_cast<std::size_t>(end), '\0');
	input.seekg(0);
	if (!contents.empty() && !input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
		throw std::runtime_error("cannot read credential file: " + path);
	}
	if (contents.empty()) {
		throw std::runtime_error("credential file is empty: " + path);
	}
	return contents;
}

std::optional<runtime_config> load_runtime_config() {
	const char *port = std::getenv("MO_SIDECAR_FLIGHT_PORT");
	if (!port || *port == '\0') {
		return std::nullopt;
	}

	runtime_config result;
	result.flight_port = parse_integer<int>("MO_SIDECAR_FLIGHT_PORT", port, 1, 65535);
	if (const char *host = std::getenv("MO_SIDECAR_FLIGHT_HOST"); host && *host) {
		result.flight_host = host;
	}
	result.flight_cert_path = required_env("MO_SIDECAR_FLIGHT_CERT");
	result.flight_key_path = required_env("MO_SIDECAR_FLIGHT_KEY");
	result.flight_client_ca_path = required_env("MO_SIDECAR_FLIGHT_CLIENT_CA");
	result.read_endpoint = parse_https_endpoint(required_env("MO_SIDECAR_READ_URL"));
	result.read_ca_path = required_env("MO_SIDECAR_READ_CA");
	result.read_client_cert_path = required_env("MO_SIDECAR_READ_CLIENT_CERT");
	result.read_client_key_path = required_env("MO_SIDECAR_READ_CLIENT_KEY");
	result.max_active_tickets = optional_integer<std::size_t>("MO_SIDECAR_MAX_ACTIVE_TICKETS", 128, 1, 4096);
	result.max_batch_bytes = optional_integer<std::uint64_t>("MO_SIDECAR_MAX_BATCH_BYTES", 64U * 1024U * 1024U,
	                                                         64U * 1024U, 1024U * 1024U * 1024U);
	result.ticket_ttl_ms = optional_integer<std::uint64_t>("MO_SIDECAR_TICKET_TTL_MS", 30'000, 1000, 10U * 60U * 1000U);

	// Fail before binding a port if any credential is absent/unreadable.
	(void)read_secret_file(result.flight_cert_path);
	(void)read_secret_file(result.flight_key_path);
	(void)read_secret_file(result.flight_client_ca_path);
	(void)read_secret_file(result.read_ca_path);
	(void)read_secret_file(result.read_client_cert_path);
	(void)read_secret_file(result.read_client_key_path);
	return result;
}

} // namespace matrixone::sidecar
