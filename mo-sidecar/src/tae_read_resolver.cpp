// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/tae_read_resolver.hpp"

#include "mo_sidecar/protocol.hpp"
#include "offload/stream_read.hpp"
#include "offload/substrait_execution.hpp"
#include "offload/tae_read.hpp"

#include "httplib.hpp"

#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace matrixone::sidecar {
namespace {

using sirius::offload::resolved_stream_read;
using sirius::offload::resolved_tae_read;
using sirius::offload::stream_read;
using sirius::offload::substrait_error_code;
using sirius::offload::substrait_execution_error;
using sirius::offload::tae_read;

[[noreturn]] void authentication_failed(const std::string &message) {
	throw substrait_execution_error(substrait_error_code::AUTHENTICATION_FAILED, message);
}

[[noreturn]] void resolution_failed(const std::string &message) {
	throw substrait_execution_error(substrait_error_code::READ_RESOLUTION_FAILED, message);
}

class temporary_manifest {
public:
	explicit temporary_manifest(std::string_view contents) {
		char pattern[] = "/tmp/mo-sidecar-manifest-XXXXXX";
#ifdef O_CLOEXEC
		const int descriptor = mkostemp(pattern, O_CLOEXEC);
#else
		const int descriptor = mkstemp(pattern);
#endif
		if (descriptor < 0) {
			resolution_failed("cannot create a private manifest file");
		}
		path_ = pattern;
		std::size_t written = 0;
		while (written < contents.size()) {
			const auto count = ::write(descriptor, contents.data() + written, contents.size() - written);
			if (count < 0 && errno == EINTR) {
				continue;
			}
			if (count <= 0) {
				::close(descriptor);
				::unlink(path_.c_str());
				resolution_failed("cannot write the private manifest file");
			}
			written += static_cast<std::size_t>(count);
		}
		if (::close(descriptor) != 0) {
			::unlink(path_.c_str());
			resolution_failed("cannot close the private manifest file");
		}
	}

	~temporary_manifest() {
		if (!path_.empty()) {
			(void)::unlink(path_.c_str());
		}
	}

	temporary_manifest(const temporary_manifest &) = delete;
	temporary_manifest &operator=(const temporary_manifest &) = delete;

	const std::string &path() const noexcept {
		return path_;
	}
	std::string release() noexcept {
		return std::exchange(path_, {});
	}

private:
	std::string path_;
};

std::string random_relation_name(std::string_view prefix) {
	std::array<unsigned char, 16> random {};
	if (RAND_bytes(random.data(), random.size()) != 1) {
		resolution_failed("cannot generate a query-local relation name");
	}
	return std::string(prefix) + hex(std::string_view(reinterpret_cast<const char *>(random.data()), random.size()));
}

class resolved_tae_relation final : public resolved_tae_read {
public:
	resolved_tae_relation(duckdb::Connection &connection, std::string relation_name, std::string manifest_path,
	                      tae_read request, ::substrait::NamedStruct canonical_schema)
	    : connection_(connection), relation_name_(std::move(relation_name)), manifest_path_(std::move(manifest_path)),
	      request_(std::move(request)), canonical_schema_(std::move(canonical_schema)) {
	}

	~resolved_tae_relation() noexcept override {
		try {
			// relation_name_ is generated from a fixed prefix and lowercase hex.
			auto result = connection_.Query("DROP VIEW IF EXISTS \"" + relation_name_ + "\"");
			(void)result;
		} catch (...) {
		}
		if (!manifest_path_.empty()) {
			(void)::unlink(manifest_path_.c_str());
		}
	}

	const std::string &relation_name() const noexcept override {
		return relation_name_;
	}
	const ::substrait::NamedStruct &canonical_schema() const noexcept override {
		return canonical_schema_;
	}
	const std::string &read_ref() const noexcept override {
		return request_.read_ref;
	}
	const std::string &query_id() const noexcept override {
		return request_.query_id;
	}
	std::uint64_t account_id() const noexcept override {
		return request_.account_id;
	}
	std::uint64_t database_id() const noexcept override {
		return request_.database_id;
	}
	std::uint64_t table_id() const noexcept override {
		return request_.table_id;
	}
	const std::string &snapshot_ts() const noexcept override {
		return request_.snapshot_ts;
	}
	const std::string &schema_digest() const noexcept override {
		return request_.schema_digest;
	}
	const std::string &manifest_sha256() const noexcept override {
		return request_.manifest_sha256;
	}
	const std::string &capability_hash() const noexcept override {
		return request_.capability_hash;
	}
	std::uint64_t expires_at_unix_ms() const noexcept override {
		return request_.expires_at_unix_ms;
	}

private:
	duckdb::Connection &connection_;
	std::string relation_name_;
	std::string manifest_path_;
	tae_read request_;
	::substrait::NamedStruct canonical_schema_;
};

class resolved_stream_relation final : public resolved_stream_read {
public:
	resolved_stream_relation(duckdb::Connection &connection, std::string relation_name,
	                         std::shared_ptr<stream_input> input, stream_read request,
	                         ::substrait::NamedStruct canonical_schema)
	    : connection_(connection), relation_name_(std::move(relation_name)), input_(std::move(input)),
	      request_(std::move(request)), canonical_schema_(std::move(canonical_schema)) {
	}

	~resolved_stream_relation() noexcept override {
		try {
			auto result = connection_.Query("DROP VIEW IF EXISTS \"" + relation_name_ + "\"");
			(void)result;
		} catch (...) {
		}
	}

	const std::string &relation_name() const noexcept override {
		return relation_name_;
	}
	const ::substrait::NamedStruct &canonical_schema() const noexcept override {
		return canonical_schema_;
	}
	const std::string &stream_ref() const noexcept override {
		return request_.stream_ref;
	}
	const std::string &query_id() const noexcept override {
		return request_.query_id;
	}
	std::uint64_t account_id() const noexcept override {
		return request_.account_id;
	}
	const std::string &snapshot_ts() const noexcept override {
		return request_.snapshot_ts;
	}
	const std::string &schema_digest() const noexcept override {
		return request_.schema_digest;
	}
	const std::string &capability_hash() const noexcept override {
		return request_.capability_hash;
	}
	std::uint64_t expires_at_unix_ms() const noexcept override {
		return request_.expires_at_unix_ms;
	}

private:
	duckdb::Connection &connection_;
	std::string relation_name_;
	std::shared_ptr<stream_input> input_;
	stream_read request_;
	::substrait::NamedStruct canonical_schema_;
};

std::string call_read_service(const runtime_config &config, const std::string &request) {
	constexpr std::size_t max_response_bytes = 64U * 1024U * 1024U;
	duckdb_httplib_openssl::SSLClient client(config.read_endpoint.host, config.read_endpoint.port,
	                                         config.read_client_cert_path, config.read_client_key_path);
	if (!client.is_valid()) {
		resolution_failed("cannot initialize the MatrixOne mTLS client");
	}
	client.set_ca_cert_path(config.read_ca_path);
	client.enable_server_certificate_verification(true);
	client.set_follow_location(false);
	client.set_connection_timeout(std::chrono::seconds(5));
	client.set_read_timeout(std::chrono::seconds(30));
	client.set_write_timeout(std::chrono::seconds(10));

	std::string response_body;
	response_body.reserve(64U * 1024U);
	bool response_too_large = false;
	const duckdb_httplib_openssl::Headers headers = {{"Accept", "application/x-protobuf"}};
	const auto response = client.Post(config.read_endpoint.path, headers, request, "application/x-protobuf",
	                                  [&](const char *data, std::size_t size) {
		                                  if (size > max_response_bytes - response_body.size()) {
			                                  response_too_large = true;
			                                  return false;
		                                  }
		                                  response_body.append(data, size);
		                                  return true;
	                                  });
	if (response_too_large) {
		resolution_failed("MatrixOne read response exceeds 64 MiB");
	}
	if (!response) {
		resolution_failed("MatrixOne read service request failed");
	}
	if (response->status == 401 || response->status == 403) {
		authentication_failed("MatrixOne rejected the sidecar workload identity");
	}
	if (response->status != 200) {
		resolution_failed("MatrixOne read service returned HTTP " + std::to_string(response->status));
	}
	const auto content_type = response->get_header_value("Content-Type");
	if (content_type != "application/x-protobuf" && !content_type.starts_with("application/x-protobuf;")) {
		resolution_failed("MatrixOne read service returned an unexpected content type");
	}
	return response_body;
}

} // namespace

matrixone_tae_read_resolver::matrixone_tae_read_resolver(duckdb::Connection &connection, const runtime_config &config,
                                                         std::string query_id, std::uint64_t account_id,
                                                         stream_input_registry &stream_inputs)
    : connection_(connection), config_(config), query_id_(std::move(query_id)), account_id_(account_id),
      stream_inputs_(stream_inputs) {
}

std::unique_ptr<resolved_tae_read>
matrixone_tae_read_resolver::resolve(const tae_read &request, const ::substrait::NamedStruct &requested_schema) {
	if (request.query_id != query_id_ || request.account_id != account_id_) {
		authentication_failed("TaeRead identity does not match the Flight execution");
	}
	if (request.capability_hash != capability_hash()) {
		authentication_failed("TaeRead capability hash does not match this sidecar");
	}
	std::string requested_schema_bytes;
	if (!requested_schema.SerializeToString(&requested_schema_bytes)) {
		resolution_failed("cannot serialize the requested TAE schema");
	}
	if (sha256_bytes(requested_schema_bytes) != request.schema_digest) {
		authentication_failed("TaeRead schema digest does not match the requested schema");
	}

	const auto canonical_read = serialize_tae_read(request);
	const auto wire_request = serialize_resolve_request(request, requested_schema_bytes);
	resolve_response response;
	try {
		response = parse_resolve_response(call_read_service(config_, wire_request));
	} catch (const substrait_execution_error &) {
		throw;
	} catch (const std::exception &error) {
		resolution_failed(std::string("invalid MatrixOne read response: ") + error.what());
	}

	if (response.tae_read != canonical_read) {
		authentication_failed("MatrixOne read response does not echo the requested TaeRead");
	}
	if (sha256_bytes(response.manifest) != request.manifest_sha256) {
		authentication_failed("MatrixOne read response has the wrong manifest digest");
	}
	if (response.canonical_schema != requested_schema_bytes ||
	    sha256_bytes(response.canonical_schema) != request.schema_digest) {
		authentication_failed("MatrixOne read response has the wrong canonical schema");
	}

	::substrait::NamedStruct canonical_schema;
	if (!canonical_schema.ParseFromString(response.canonical_schema)) {
		resolution_failed("MatrixOne read response contains a malformed canonical schema");
	}

	temporary_manifest manifest(response.manifest);
	const auto relation_name = random_relation_name("__mo_tae_");
	try {
		connection_.TableFunction("tae_scan", {duckdb::Value(manifest.path())})->CreateView(relation_name, true, true);
	} catch (const std::exception &error) {
		resolution_failed(std::string("cannot bind the authenticated TAE manifest: ") + error.what());
	}

	return std::make_unique<resolved_tae_relation>(connection_, relation_name, manifest.release(), request,
	                                               std::move(canonical_schema));
}

std::unique_ptr<resolved_stream_read>
matrixone_tae_read_resolver::resolve(const stream_read &request, const ::substrait::NamedStruct &requested_schema) {
	if (request.query_id != query_id_ || request.account_id != account_id_) {
		authentication_failed("StreamRead identity does not match the Flight execution");
	}
	if (request.capability_hash != capability_hash()) {
		authentication_failed("StreamRead capability hash does not match this sidecar");
	}
	std::string requested_schema_bytes;
	if (!requested_schema.SerializeToString(&requested_schema_bytes)) {
		resolution_failed("cannot serialize the requested stream schema");
	}
	if (sha256_bytes(requested_schema_bytes) != request.schema_digest) {
		authentication_failed("StreamRead schema digest does not match the requested schema");
	}

	auto input = stream_inputs_.create(request, requested_schema);
	const auto relation_name = random_relation_name("__mo_stream_");
	try {
		connection_.TableFunction("mo_stream_scan", {duckdb::Value::POINTER(reinterpret_cast<uintptr_t>(input.get()))})
		    ->CreateView(relation_name, true, true);
	} catch (const std::exception &error) {
		input->cancel("StreamRead relation binding failed");
		resolution_failed(std::string("cannot bind the authenticated StreamRead: ") + error.what());
	}
	return std::make_unique<resolved_stream_relation>(connection_, relation_name, std::move(input), request,
	                                                  requested_schema);
}

} // namespace matrixone::sidecar
