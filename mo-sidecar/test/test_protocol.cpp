// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "catch.hpp"

#include "mo_sidecar/config.hpp"
#include "mo_sidecar/protocol.hpp"
#include "offload/tae_read.hpp"

namespace {

void varint(std::string &output, std::uint64_t value) {
	while (value >= 0x80U) {
		output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
		value >>= 7U;
	}
	output.push_back(static_cast<char>(value));
}

void integer(std::string &output, unsigned field, std::uint64_t value) {
	varint(output, static_cast<std::uint64_t>(field) << 3U);
	varint(output, value);
}

void bytes(std::string &output, unsigned field, std::string_view value) {
	varint(output, (static_cast<std::uint64_t>(field) << 3U) | 2U);
	varint(output, value.size());
	output.append(value);
}

std::string valid_execute_request() {
	std::string result;
	integer(result, 1, 1);
	bytes(result, 2, "0.78.0");
	bytes(result, 3, matrixone::sidecar::capability_hash());
	integer(result, 4, 1024 * 1024);
	integer(result, 5, 123456789);
	bytes(result, 6, "plan");
	return result;
}

} // namespace

TEST_CASE("ExecuteSubstrait envelope parses strictly", "[sidecar][protocol]") {
	const auto parsed = matrixone::sidecar::parse_execute_request(valid_execute_request());
	REQUIRE(parsed.protocol_version == 1);
	REQUIRE(parsed.substrait_version == "0.78.0");
	REQUIRE(parsed.capability_hash == matrixone::sidecar::capability_hash());
	REQUIRE(parsed.max_batch_bytes == 1024 * 1024);
	REQUIRE(parsed.deadline_unix_ms == 123456789);
	REQUIRE(parsed.plan == "plan");
}

TEST_CASE("ExecuteSubstrait rejects duplicates, unknowns, truncation, and wrong wire types", "[sidecar][protocol]") {
	auto duplicate = valid_execute_request();
	integer(duplicate, 1, 1);
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(duplicate));

	auto unknown = valid_execute_request();
	integer(unknown, 7, 1);
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(unknown));

	auto truncated = valid_execute_request();
	truncated.pop_back();
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(truncated));

	std::string wrong_wire;
	bytes(wrong_wire, 1, "1");
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(wrong_wire));
}

TEST_CASE("ResolveTaeRead response is strict and bounded", "[sidecar][protocol]") {
	std::string wire;
	bytes(wire, 1, "read");
	bytes(wire, 2, "manifest");
	bytes(wire, 3, "schema");
	const auto parsed = matrixone::sidecar::parse_resolve_response(wire);
	REQUIRE(parsed.tae_read == "read");
	REQUIRE(parsed.manifest == "manifest");
	REQUIRE(parsed.canonical_schema == "schema");

	auto duplicate = wire;
	bytes(duplicate, 2, "again");
	REQUIRE_THROWS(matrixone::sidecar::parse_resolve_response(duplicate));
}

TEST_CASE("TaeRead serialization includes the database identity", "[sidecar][protocol]") {
	sirius::offload::tae_read request;
	request.protocol_version = 1;
	request.read_ref = "read";
	request.query_id = "query";
	request.account_id = 7;
	request.database_id = 11;
	request.table_id = 13;
	request.snapshot_ts = std::string(12, 's');
	request.schema_digest = std::string(32, 'd');
	request.manifest_sha256 = std::string(32, 'm');
	request.capability_hash = std::string(32, 'c');
	request.expires_at_unix_ms = 17;

	std::string expected;
	integer(expected, 1, 1);
	bytes(expected, 3, request.read_ref);
	bytes(expected, 4, request.query_id);
	integer(expected, 5, request.account_id);
	integer(expected, 6, request.table_id);
	bytes(expected, 7, request.snapshot_ts);
	bytes(expected, 8, request.schema_digest);
	bytes(expected, 9, request.manifest_sha256);
	bytes(expected, 10, request.capability_hash);
	integer(expected, 11, request.expires_at_unix_ms);
	integer(expected, 12, request.database_id);
	REQUIRE(matrixone::sidecar::serialize_tae_read(request) == expected);
}

TEST_CASE("HTTPS endpoint parsing fails closed", "[sidecar][config]") {
	auto endpoint =
	    matrixone::sidecar::parse_https_endpoint("https://matrixone.internal:9443/internal/v1/sidecar/read/resolve");
	REQUIRE(endpoint.host == "matrixone.internal");
	REQUIRE(endpoint.port == 9443);
	REQUIRE(endpoint.path == "/internal/v1/sidecar/read/resolve");

	endpoint = matrixone::sidecar::parse_https_endpoint("https://[::1]:9443/resolve");
	REQUIRE(endpoint.host == "::1");
	REQUIRE(endpoint.port == 9443);
	REQUIRE_THROWS(matrixone::sidecar::parse_https_endpoint("http://localhost/resolve"));
	REQUIRE_THROWS(matrixone::sidecar::parse_https_endpoint("https://user@localhost/resolve"));
	REQUIRE_THROWS(matrixone::sidecar::parse_https_endpoint("https://localhost:70000/resolve"));
}

TEST_CASE("Capability document has a stable SHA-256", "[sidecar][protocol]") {
	REQUIRE(matrixone::sidecar::capability_hash().size() == 32);
	REQUIRE(matrixone::sidecar::hex(matrixone::sidecar::capability_hash()) ==
	        "52a6be3f72a10804fee3840b0ea30d225715ad23a321630f063372ee815bc272");
	REQUIRE(matrixone::sidecar::sha256_bytes(matrixone::sidecar::capability_document()) ==
	        matrixone::sidecar::capability_hash());
}
