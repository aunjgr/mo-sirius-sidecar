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

std::string valid_execute_request(std::uint64_t account_id = 42, bool include_account = true) {
	const auto query_id = std::string(16, 'q');
	const auto plan = std::string("plan");
	std::string result;
	integer(result, 1, matrixone::sidecar::k_protocol_version);
	bytes(result, 2, "0.78.0");
	bytes(result, 3, matrixone::sidecar::capability_hash());
	integer(result, 4, 1024 * 1024);
	integer(result, 5, 123456789);
	bytes(result, 6, plan);
	bytes(result, 7, query_id);
	bytes(result, 8, matrixone::sidecar::execution_idempotency_key(account_id, query_id));
	if (include_account) {
		integer(result, 9, account_id);
	}
	integer(result, 10, matrixone::sidecar::k_max_stream_input_batch_bytes);
	return result;
}

} // namespace

TEST_CASE("ExecuteSubstrait envelope parses strictly", "[sidecar][protocol]") {
	const auto parsed = matrixone::sidecar::parse_execute_request(valid_execute_request());
	REQUIRE(parsed.protocol_version == matrixone::sidecar::k_protocol_version);
	REQUIRE(parsed.substrait_version == "0.78.0");
	REQUIRE(parsed.capability_hash == matrixone::sidecar::capability_hash());
	REQUIRE(parsed.max_batch_bytes == 1024 * 1024);
	REQUIRE(parsed.deadline_unix_ms == 123456789);
	REQUIRE(parsed.plan == "plan");
	REQUIRE(parsed.query_id == std::string(16, 'q'));
	REQUIRE(parsed.idempotency_key == matrixone::sidecar::execution_idempotency_key(42, std::string(16, 'q')));
	REQUIRE(parsed.account_id == 42);
	REQUIRE(parsed.max_input_batch_bytes == matrixone::sidecar::k_max_stream_input_batch_bytes);
	REQUIRE(matrixone::sidecar::hex(parsed.idempotency_key) ==
	        "77f6a676cc4bfdbc9265e1bbbcd8140f4a820ec41a2979f52706f41ff22fb33a");
}

TEST_CASE("ExecuteSubstrait distinguishes the system account from a missing identity", "[sidecar][protocol]") {
	const auto parsed = matrixone::sidecar::parse_execute_request(valid_execute_request(0));
	REQUIRE(parsed.account_id == 0);
	REQUIRE(parsed.idempotency_key == matrixone::sidecar::execution_idempotency_key(0, std::string(16, 'q')));
	REQUIRE(parsed.idempotency_key != matrixone::sidecar::execution_idempotency_key(1, std::string(16, 'q')));
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(valid_execute_request(0, false)));
}

TEST_CASE("ExecuteSubstrait rejects duplicates, unknowns, truncation, and "
          "wrong wire types",
          "[sidecar][protocol]") {
	auto duplicate = valid_execute_request();
	integer(duplicate, 1, 1);
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(duplicate));

	auto unknown = valid_execute_request();
	integer(unknown, 11, 1);
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(unknown));

	auto truncated = valid_execute_request();
	truncated.pop_back();
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(truncated));

	std::string wrong_wire;
	bytes(wrong_wire, 1, "1");
	REQUIRE_THROWS(matrixone::sidecar::parse_execute_request(wrong_wire));
}

TEST_CASE("UploadInput and MO native frames parse strictly", "[sidecar][protocol]") {
	std::string upload;
	bytes(upload, 1, std::string(32, 't'));
	bytes(upload, 2, std::string(32, 'r'));
	const auto request = matrixone::sidecar::parse_upload_input_request(upload);
	REQUIRE(request.ticket == std::string(32, 't'));
	REQUIRE(request.stream_ref == std::string(32, 'r'));
	std::string ready_ack;
	integer(ready_ack, 6, 1);
	REQUIRE(matrixone::sidecar::serialize_upload_input_ack({.ready = true}) == ready_ack);

	std::string frame("MOB1", 4);
	frame.push_back(1);
	frame.push_back(0);
	frame.push_back(0);
	frame.push_back(0);
	for (unsigned i = 0; i < 8; ++i) {
		frame.push_back(static_cast<char>((std::uint64_t {7} >> (i * 8U)) & 0xffU));
	}
	for (unsigned i = 0; i < 8; ++i) {
		frame.push_back(static_cast<char>((std::uint64_t {3} >> (i * 8U)) & 0xffU));
	}
	frame.append("bat");
	const auto parsed = matrixone::sidecar::parse_native_batch_frame(frame);
	REQUIRE(parsed.sequence == 7);
	REQUIRE(parsed.payload == "bat");

	auto bad_magic = frame;
	bad_magic[0] = 'X';
	REQUIRE_THROWS(matrixone::sidecar::parse_native_batch_frame(bad_magic));
	auto bad_size = frame;
	bad_size[16] = 4;
	REQUIRE_THROWS(matrixone::sidecar::parse_native_batch_frame(bad_size));
	REQUIRE_THROWS(matrixone::sidecar::parse_native_batch_frame(frame.substr(0, 23)));
}

TEST_CASE("CancelExecution accepts exactly one opaque identity", "[sidecar][protocol]") {
	std::string by_ticket;
	bytes(by_ticket, 1, std::string(32, 't'));
	const auto ticket = matrixone::sidecar::parse_cancel_request(by_ticket);
	REQUIRE(ticket.ticket == std::string(32, 't'));
	REQUIRE(ticket.idempotency_key.empty());

	std::string by_idempotency;
	bytes(by_idempotency, 2, std::string(32, 'i'));
	const auto idempotency = matrixone::sidecar::parse_cancel_request(by_idempotency);
	REQUIRE(idempotency.ticket.empty());
	REQUIRE(idempotency.idempotency_key == std::string(32, 'i'));

	auto both = by_ticket + by_idempotency;
	REQUIRE_THROWS(matrixone::sidecar::parse_cancel_request(both));
	std::string short_ticket;
	bytes(short_ticket, 1, "short");
	REQUIRE_THROWS(matrixone::sidecar::parse_cancel_request(short_ticket));
	REQUIRE_THROWS(matrixone::sidecar::parse_cancel_request(std::string {}));
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
	request.protocol_version = matrixone::sidecar::k_tae_read_protocol_version;
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
	integer(expected, 1, matrixone::sidecar::k_tae_read_protocol_version);
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

	request.account_id = 0;
	expected.clear();
	integer(expected, 1, matrixone::sidecar::k_tae_read_protocol_version);
	bytes(expected, 3, request.read_ref);
	bytes(expected, 4, request.query_id);
	integer(expected, 5, 0);
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
	        "cf5b91ce5ad2c29a952df10905d025ee143e0481f9eb2f28e6d129b81d673dc1");
	REQUIRE(matrixone::sidecar::sha256_bytes(matrixone::sidecar::capability_document()) ==
	        matrixone::sidecar::capability_hash());
}
