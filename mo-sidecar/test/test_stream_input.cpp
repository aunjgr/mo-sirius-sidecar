// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "catch.hpp"

#include "mo_sidecar/native_result.hpp"
#include "mo_sidecar/stream_input.hpp"

#include <arrow/buffer.h>
#include <arrow/flight/server.h>
#include <arrow/ipc/dictionary.h>
#include <arrow/ipc/writer.h>
#include <duckdb.hpp>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/parser/parsed_data/create_table_function_info.hpp>

#include <chrono>
#include <cstring>
#include <future>
#include <string>

namespace {

void u32(std::string &output, std::uint32_t value) {
	for (unsigned i = 0; i < 4; ++i) {
		output.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
	}
}

void u64(std::string &output, std::uint64_t value) {
	for (unsigned i = 0; i < 8; ++i) {
		output.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
	}
}

::substrait::NamedStruct i64_schema() {
	::substrait::NamedStruct schema;
	schema.add_names("value");
	schema.mutable_struct_()->add_types()->mutable_i64();
	return schema;
}

sirius::offload::stream_read stream_request() {
	sirius::offload::stream_read request;
	request.protocol_version = matrixone::sidecar::k_stream_read_protocol_version;
	request.stream_ref = std::string(32, 'r');
	request.query_id = std::string(16, 'q');
	request.snapshot_ts = std::string(12, 's');
	request.schema_digest = std::string(32, 'd');
	request.capability_hash = std::string(32, 'c');
	request.expires_at_unix_ms = 2000;
	return request;
}

std::string vector_bytes(tae::MOTypeOid oid, std::int32_t size, std::int32_t width, std::int32_t scale,
						 std::uint32_t rows, std::string data, std::string area = {}, std::string nulls = {}) {
	std::string result;
	result.push_back(0);
	tae::MOType type{};
	type.oid = oid;
	type.size = size;
	type.width = width;
	type.scale = scale;
	result.append(reinterpret_cast<const char *>(&type), sizeof(type));
	u32(result, rows);
	u32(result, static_cast<std::uint32_t>(data.size()));
	result.append(data);
	u32(result, static_cast<std::uint32_t>(area.size()));
	result.append(area);
	u32(result, static_cast<std::uint32_t>(nulls.size()));
	result.append(nulls);
	result.push_back(0);
	return result;
}

std::shared_ptr<arrow::Buffer> native_frame(std::uint64_t sequence, std::uint64_t rows,
											const std::vector<std::string> &vectors) {
	std::string batch;
	u64(batch, rows);
	u32(batch, static_cast<std::uint32_t>(vectors.size()));
	for (const auto &vector : vectors) {
		u32(batch, static_cast<std::uint32_t>(vector.size()));
		batch.append(vector);
	}
	u32(batch, 0);
	u32(batch, 0);
	u32(batch, 0);
	u32(batch, 0);

	std::string frame("MOB1", 4);
	frame.push_back(1);
	frame.push_back(0);
	frame.push_back(0);
	frame.push_back(0);
	u64(frame, sequence);
	u64(frame, batch.size());
	frame.append(batch);
	return arrow::Buffer::FromString(std::move(frame));
}

std::shared_ptr<arrow::Buffer> i64_frame(std::uint64_t sequence, std::int64_t first, std::int64_t second) {
	std::string data;
	u64(data, static_cast<std::uint64_t>(first));
	u64(data, static_cast<std::uint64_t>(second));
	return native_frame(sequence, 2, {vector_bytes(tae::MO_T_int64, 8, 0, 0, 2, std::move(data))});
}

::substrait::NamedStruct tpch_schema() {
	::substrait::NamedStruct schema;
	for (const auto *name : {"s", "d64", "d128", "day"}) {
		schema.add_names(name);
	}
	schema.mutable_struct_()->add_types()->mutable_varchar();
	auto *d64 = schema.mutable_struct_()->add_types()->mutable_decimal();
	d64->set_precision(18);
	d64->set_scale(2);
	auto *d128 = schema.mutable_struct_()->add_types()->mutable_decimal();
	d128->set_precision(38);
	d128->set_scale(4);
	schema.mutable_struct_()->add_types()->mutable_date();
	return schema;
}

std::shared_ptr<arrow::Buffer> tpch_frame() {
	std::string strings;
	tae::Varlena inline_value{};
	inline_value.data[0] = 5;
	std::memcpy(inline_value.data + 1, "hello", 5);
	strings.append(reinterpret_cast<const char *>(&inline_value), sizeof(inline_value));
	tae::Varlena big_value{};
	const std::uint32_t marker = tae::VARLENA_BIG_MARKER;
	const std::uint32_t offset = 0;
	const std::string area = "this value is deliberately longer than twenty three bytes";
	const auto length = static_cast<std::uint32_t>(area.size());
	std::memcpy(big_value.data, &marker, sizeof(marker));
	std::memcpy(big_value.data + 4, &offset, sizeof(offset));
	std::memcpy(big_value.data + 8, &length, sizeof(length));
	strings.append(reinterpret_cast<const char *>(&big_value), sizeof(big_value));
	strings.append(sizeof(tae::Varlena), '\0');
	std::string nulls;
	u64(nulls, 1);
	u64(nulls, 3);
	u64(nulls, 8);
	u64(nulls, 4);

	std::string decimal64;
	u64(decimal64, 1234);
	u64(decimal64, static_cast<std::uint64_t>(std::int64_t{-567}));
	u64(decimal64, 0);
	std::string decimal128;
	for (const std::uint64_t value : {123456U, 1U, 0U}) {
		u64(decimal128, value);
		u64(decimal128, 0);
	}
	std::string dates;
	for (const std::int32_t value :
		 {tae::MO_UNIX_EPOCH_DAYS, tae::MO_UNIX_EPOCH_DAYS + 1, tae::MO_UNIX_EPOCH_DAYS - 1}) {
		u32(dates, static_cast<std::uint32_t>(value));
	}
	return native_frame(1, 3,
						{vector_bytes(tae::MO_T_varchar, -24, 0, 0, 3, std::move(strings), area, std::move(nulls)),
						 vector_bytes(tae::MO_T_decimal64, 8, 18, 2, 3, std::move(decimal64)),
						 vector_bytes(tae::MO_T_decimal128, 16, 38, 4, 3, std::move(decimal128)),
						 vector_bytes(tae::MO_T_date, 4, 0, 0, 3, std::move(dates))});
}

} // namespace

TEST_CASE("MO native batch decoder validates and exposes borrowed vectors", "[sidecar][stream]") {
	auto buffer = i64_frame(1, -7, 42);
	const std::string_view bytes(reinterpret_cast<const char *>(buffer->data()), buffer->size());
	auto envelope = matrixone::sidecar::parse_native_batch_frame(bytes);
	matrixone::sidecar::native_batch_view batch(buffer, envelope, i64_schema());
	REQUIRE(batch.sequence() == 1);
	REQUIRE(batch.rows() == 2);
	REQUIRE(batch.columns().size() == 1);
	std::int64_t first = 0;
	std::int64_t second = 0;
	std::memcpy(&first, batch.columns()[0].data.data(), sizeof(first));
	std::memcpy(&second, batch.columns()[0].data.data() + sizeof(first), sizeof(second));
	REQUIRE(first == -7);
	REQUIRE(second == 42);
	REQUIRE_FALSE(batch.columns()[0].is_null(0));

	auto malformed = arrow::Buffer::FromString(std::string("MOB1", 4));
	REQUIRE_THROWS(matrixone::sidecar::native_batch_view(
		malformed, matrixone::sidecar::native_batch_frame{1, std::string_view("bad")}, i64_schema()));
}

TEST_CASE("MO native input acknowledges only when Sirius releases the current frame", "[sidecar][stream]") {
	auto input = std::make_shared<matrixone::sidecar::stream_input>(stream_request(), i64_schema());
	REQUIRE(input->attach().ok());
	auto published =
		std::async(std::launch::async, [&] { return input->publish(i64_frame(1, 10, 20), [] { return false; }); });
	auto batch = input->next_batch();
	REQUIRE(batch);
	REQUIRE(published.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
	input->mark_consumed(batch->sequence());
	auto ack = published.get();
	REQUIRE(ack.ok());
	REQUIRE(ack->acknowledged_batches == 1);
	REQUIRE(ack->rows == 2);
	REQUIRE_FALSE(ack->complete);

	auto prefetched = std::async(std::launch::async,
		[&] { return input->publish(i64_frame(2, 30, 40), [] { return false; }); });
	auto second = input->next_batch();
	REQUIRE(second);
	REQUIRE(second->sequence() == 2);
	REQUIRE(prefetched.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
	input->mark_consumed(second->sequence());
	auto second_ack = prefetched.get();
	REQUIRE(second_ack.ok());
	REQUIRE(second_ack->acknowledged_batches == 2);
	REQUIRE(second_ack->rows == 4);

	auto finishing = std::async(std::launch::async, [&] { return input->finish_upload([] { return false; }); });
	REQUIRE(input->next_batch() == nullptr);
	auto complete = finishing.get();
	REQUIRE(complete.ok());
	REQUIRE(complete->complete);
	REQUIRE_FALSE(complete->not_needed);
}

TEST_CASE("MO native input cancellation releases a blocked publisher", "[sidecar][stream]") {
	auto input = std::make_shared<matrixone::sidecar::stream_input>(stream_request(), i64_schema());
	REQUIRE(input->attach().ok());
	auto published =
		std::async(std::launch::async, [&] { return input->publish(i64_frame(1, 10, 20), [] { return false; }); });
	REQUIRE(input->next_batch());
	input->cancel("injected cancellation");
	auto result = published.get();
	REQUIRE_FALSE(result.ok());
	REQUIRE(result.status().IsCancelled());
}

TEST_CASE("mo_stream_scan materializes TPCH MO native types", "[sidecar][stream]") {
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	connection.BeginTransaction();
	auto &catalog = duckdb::Catalog::GetSystemCatalog(*connection.context);
	duckdb::CreateTableFunctionInfo info(matrixone::sidecar::get_stream_scan_function());
	catalog.CreateTableFunction(*connection.context, info);
	connection.Commit();

	auto input = std::make_shared<matrixone::sidecar::stream_input>(stream_request(), tpch_schema());
	REQUIRE(input->attach().ok());
	auto query = std::async(std::launch::async, [&] {
		return connection
			.TableFunction("mo_stream_scan", {duckdb::Value::POINTER(reinterpret_cast<std::uintptr_t>(input.get()))})
			->Execute();
	});
	auto published = input->publish(tpch_frame(), [] { return false; });
	REQUIRE(published.ok());
	REQUIRE(published->acknowledged_batches == 1);
	auto finishing = std::async(std::launch::async, [&] { return input->finish_upload([] { return false; }); });
	auto result = query.get();
	auto complete = finishing.get();
	REQUIRE(complete.ok());
	REQUIRE(result);
	REQUIRE_FALSE(result->HasError());
	auto chunk = result->Fetch();
	REQUIRE(chunk);
	REQUIRE(chunk->size() == 3);
	REQUIRE(chunk->GetValue(0, 0).ToString() == "hello");
	REQUIRE(chunk->GetValue(0, 1).ToString() == "this value is deliberately longer than twenty three bytes");
	REQUIRE(chunk->GetValue(0, 2).IsNull());
	REQUIRE(chunk->GetValue(1, 0).ToString() == "12.34");
	REQUIRE(chunk->GetValue(1, 1).ToString() == "-5.67");
	REQUIRE(chunk->GetValue(2, 0).ToString() == "12.3456");
	REQUIRE(chunk->GetValue(3, 0).ToString() == "1970-01-01");
	REQUIRE(chunk->GetValue(3, 1).ToString() == "1970-01-02");
}

TEST_CASE("MO native result encoder round trips through the input codec", "[sidecar][stream][result]") {
	duckdb::vector<duckdb::LogicalType> types{duckdb::LogicalType::VARCHAR, duckdb::LogicalType::BIGINT,
											  duckdb::LogicalType::DATE};
	duckdb::DataChunk chunk;
	chunk.Initialize(duckdb::Allocator::DefaultAllocator(), types);
	chunk.SetValue(0, 0, duckdb::Value("short"));
	chunk.SetValue(0, 1, duckdb::Value("this string is longer than the MatrixOne inline limit"));
	chunk.SetValue(1, 0, duckdb::Value::BIGINT(-7));
	chunk.SetValue(1, 1, duckdb::Value());
	chunk.SetValue(2, 0, duckdb::Value::DATE(duckdb::date_t(0)));
	chunk.SetValue(2, 1, duckdb::Value::DATE(duckdb::date_t(1)));
	chunk.SetCardinality(2);

	matrixone::sidecar::native_result_schema schema{matrixone::sidecar::k_native_result_schema_version,
													{{"s", tae::MO_T_varchar, 100, 0, 0, false},
													 {"i", tae::MO_T_int64, 0, 0, 0, false},
													 {"d", tae::MO_T_date, 0, 0, 0, false}}};
	sirius::offload::execution_schema actual{{"s", "i", "d"}, types};
	REQUIRE_NOTHROW(matrixone::sidecar::validate_native_result_schema(schema, actual));

	const auto payload = matrixone::sidecar::encode_native_result_batch(chunk, 0, 2, schema);
	::substrait::NamedStruct input_schema;
	input_schema.add_names("s");
	input_schema.add_names("i");
	input_schema.add_names("d");
	input_schema.mutable_struct_()->add_types()->mutable_string();
	input_schema.mutable_struct_()->add_types()->mutable_i64();
	input_schema.mutable_struct_()->add_types()->mutable_date();
	const auto frame_bytes = matrixone::sidecar::serialize_native_batch_frame(1, payload);
	auto frame = arrow::Buffer::FromString(frame_bytes);
	auto envelope = matrixone::sidecar::parse_native_batch_frame(frame_bytes);
	matrixone::sidecar::native_batch_view decoded(frame, envelope, input_schema);
	REQUIRE(decoded.rows() == 2);
	REQUIRE(decoded.columns().size() == 3);
	REQUIRE(decoded.columns()[0].type.oid == tae::MO_T_varchar);
	REQUIRE(decoded.columns()[0].area == "this string is longer than the MatrixOne inline limit");
	REQUIRE(decoded.columns()[1].is_null(1));
	std::int32_t first_date = 0;
	std::memcpy(&first_date, decoded.columns()[2].data.data(), sizeof(first_date));
	REQUIRE(first_date == tae::MO_UNIX_EPOCH_DAYS);
}

TEST_CASE("Flight accepts native data-header payloads", "[sidecar][stream][result]") {
	arrow::flight::FlightPayload schema_payload;
	auto schema = arrow::schema({});
	arrow::ipc::DictionaryFieldMapper mapper(*schema);
	REQUIRE(arrow::ipc::GetSchemaPayload(*schema, arrow::ipc::IpcWriteOptions::Defaults(), mapper,
										 &schema_payload.ipc_message)
				.ok());
	REQUIRE(schema_payload.Validate().ok());
	auto frame = matrixone::sidecar::serialize_native_batch_frame(1, "batch");
	arrow::flight::FlightPayload batch_payload;
	batch_payload.ipc_message.type = arrow::ipc::MessageType::RECORD_BATCH;
	batch_payload.ipc_message.metadata = arrow::Buffer::FromString(frame);
	REQUIRE(batch_payload.Validate().ok());
}

TEST_CASE("MO native result schema accepts bounded TPCH coercions", "[sidecar][stream][result]") {
	matrixone::sidecar::native_result_schema schema{matrixone::sidecar::k_native_result_schema_version,
													{{"avg_decimal", tae::MO_T_decimal128, 38, 8, 0, false},
													 {"sum_integer", tae::MO_T_decimal128, 38, 0, 0, false}}};
	sirius::offload::execution_schema actual{{"avg_decimal", "sum_integer"},
											 {duckdb::LogicalType::DOUBLE, duckdb::LogicalType::HUGEINT}};
	REQUIRE_NOTHROW(matrixone::sidecar::validate_native_result_schema(schema, actual));

	actual.types[1] = duckdb::LogicalType::DOUBLE;
	schema.columns[1].scale = 1;
	REQUIRE_NOTHROW(matrixone::sidecar::validate_native_result_schema(schema, actual));
	actual.types[1] = duckdb::LogicalType::HUGEINT;
	REQUIRE_THROWS(matrixone::sidecar::validate_native_result_schema(schema, actual));
}
