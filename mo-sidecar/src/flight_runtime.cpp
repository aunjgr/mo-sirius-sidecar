// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "mo_sidecar/flight_runtime.hpp"

#include "mo_sidecar/protocol.hpp"
#include "mo_sidecar/tae_read_resolver.hpp"

#include "execution/sirius_execution_evidence.hpp"
#include "offload/substrait_execution.hpp"

#include <arrow/array/data.h>
#include <arrow/c/bridge.h>
#include <arrow/flight/api.h>
#include <arrow/flight/server.h>
#include <arrow/ipc/writer.h>
#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <duckdb/common/arrow/arrow_converter.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace matrixone::sidecar {
namespace {

namespace flight = arrow::flight;

arrow::Status flight_error(flight::FlightStatusCode code, const std::string &message, std::string detail = {}) {
	return flight::MakeFlightError(code, message, std::move(detail));
}

arrow::Status substrait_error(const sirius::offload::substrait_execution_error &error) {
	using code = sirius::offload::substrait_error_code;
	switch (error.code()) {
	case code::UNSUPPORTED_PLAN:
		return flight_error(flight::FlightStatusCode::Failed, error.what(), "UNSUPPORTED_PLAN");
	case code::INVALID_PLAN:
		return arrow::Status::Invalid(error.what());
	case code::AUTHENTICATION_FAILED:
		return flight_error(flight::FlightStatusCode::Unauthorized, error.what(), "AUTHENTICATION_FAILED");
	case code::READ_RESOLUTION_FAILED:
		return flight_error(flight::FlightStatusCode::Unavailable, error.what(), "READ_RESOLUTION_FAILED");
	case code::CANCELLED:
		return flight_error(flight::FlightStatusCode::Cancelled, error.what(), "CANCELLED");
	case code::EXECUTION_FAILED:
		return flight_error(flight::FlightStatusCode::Internal, error.what(), "EXECUTION_FAILED");
	}
	return flight_error(flight::FlightStatusCode::Internal, "unknown Sirius execution error");
}

std::shared_ptr<arrow::Schema> make_arrow_schema(duckdb::ClientContext &context,
                                                 const sirius::offload::execution_schema &schema) {
	ArrowSchema c_schema {};
	auto properties = context.GetClientProperties();
	duckdb::ArrowConverter::ToArrowSchema(&c_schema, schema.types, schema.names, properties);
	auto imported = arrow::ImportSchema(&c_schema);
	if (!imported.ok()) {
		throw std::runtime_error(imported.status().ToString());
	}
	return std::move(imported).ValueOrDie();
}

void add_array_bytes(const std::shared_ptr<arrow::ArrayData> &data, std::unordered_set<const arrow::Buffer *> &seen,
                     std::uint64_t &total) {
	if (!data) {
		return;
	}
	for (const auto &buffer : data->buffers) {
		if (buffer && seen.emplace(buffer.get()).second) {
			const auto size = static_cast<std::uint64_t>(buffer->size());
			if (size > std::numeric_limits<std::uint64_t>::max() - total) {
				throw std::overflow_error("Arrow batch size overflow");
			}
			total += size;
		}
	}
	for (const auto &child : data->child_data) {
		add_array_bytes(child, seen, total);
	}
	add_array_bytes(data->dictionary, seen, total);
}

std::uint64_t batch_bytes(const arrow::RecordBatch &batch) {
	std::unordered_set<const arrow::Buffer *> seen;
	std::uint64_t total = 0;
	for (const auto &column : batch.column_data()) {
		add_array_bytes(column, seen, total);
	}
	return total;
}

std::shared_ptr<arrow::RecordBatch> convert_chunk(const duckdb::DataChunk &chunk, duckdb::ClientContext &context,
                                                  const std::shared_ptr<arrow::Schema> &schema) {
	ArrowArray c_array {};
	// ArrowConverter copies the chunk into Arrow-owned buffers. Its legacy API is
	// non-const even though Append does not mutate the input chunk.
	duckdb::ArrowConverter::ToArrowArray(const_cast<duckdb::DataChunk &>(chunk), &c_array,
	                                     context.GetClientProperties(), {});
	auto imported = arrow::ImportRecordBatch(&c_array, schema);
	if (!imported.ok()) {
		throw std::runtime_error(imported.status().ToString());
	}
	return std::move(imported).ValueOrDie();
}

std::string random_ticket() {
	std::string value(32, '\0');
	if (RAND_bytes(reinterpret_cast<unsigned char *>(value.data()), value.size()) != 1) {
		throw std::runtime_error("cannot generate a Flight ticket");
	}
	return value;
}

class ticket_registry;

class execution_entry final : public std::enable_shared_from_this<execution_entry> {
public:
	using terminal_callback = std::function<void(const std::string &)>;

	execution_entry(duckdb::DatabaseInstance &database, const runtime_config &config, execute_request request,
	                std::string ticket, terminal_callback on_terminal)
	    : config_(config), request_(std::move(request)), ticket_(std::move(ticket)),
	      on_terminal_(std::move(on_terminal)), connection_(std::make_unique<duckdb::Connection>(database)),
	      evidence_(std::make_shared<sirius::execution_evidence>(sirius::execution_backend::SIRIUS_GPU)) {
		// Resolution creates query-local views that binding and execution must
		// observe in one transaction. The entry owns the connection through
		// quiescence; Connection rolls this read-only transaction back on destroy.
		connection_->BeginTransaction();
		matrixone_tae_read_resolver resolver(*connection_, config_, request_.query_id, request_.account_id);
		execution_ = sirius::offload::prepare_substrait(*connection_->context, request_.plan, resolver, evidence_);
		schema_ = make_arrow_schema(*connection_->context, execution_->schema());
	}

	~execution_entry() noexcept {
		cancel(false);
		std::lock_guard worker_lock(worker_mutex_);
		if (worker_.joinable()) {
			if (worker_.get_id() == std::this_thread::get_id()) {
				worker_.detach();
			} else {
				worker_.join();
			}
		}
	}

	const std::string &ticket() const noexcept {
		return ticket_;
	}
	const std::shared_ptr<arrow::Schema> &schema() const noexcept {
		return schema_;
	}
	std::uint64_t deadline_unix_ms() const noexcept {
		return request_.deadline_unix_ms;
	}
	const std::string &idempotency_key() const noexcept {
		return request_.idempotency_key;
	}
	bool replayable() {
		std::lock_guard lock(mutex_);
		return !claimed_ && !terminal_;
	}

	bool claim() {
		std::lock_guard lock(mutex_);
		if (claimed_ || terminal_) {
			return false;
		}
		claimed_ = true;
		return true;
	}

	void start() {
		std::lock_guard lock(mutex_);
		if (!claimed_ || worker_.joinable() || terminal_) {
			return;
		}
		worker_ = std::thread([self = shared_from_this()] { self->run(); });
	}

	arrow::Status read_next(const flight::ServerCallContext &context, std::shared_ptr<arrow::RecordBatch> *output) {
		std::unique_lock lock(mutex_);
		const auto deadline =
		    std::chrono::system_clock::time_point(std::chrono::milliseconds(request_.deadline_unix_ms));
		while (true) {
			lock.unlock();
			const bool client_cancelled = context.is_cancelled();
			lock.lock();
			if (client_cancelled && !terminal_) {
				lock.unlock();
				cancel(false);
				lock.lock();
			}
			if (batch_ || terminal_) {
				break;
			}
			const auto now = std::chrono::system_clock::now();
			if (now >= deadline) {
				lock.unlock();
				cancel(true);
				lock.lock();
				continue;
			}
			condition_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
		}
		if (batch_) {
			*output = std::exchange(batch_, nullptr);
			condition_.notify_all();
			return arrow::Status::OK();
		}
		*output = nullptr;
		return terminal_status_;
	}

	bool cancel(bool timed_out) {
		bool notify_terminal = false;
		bool unstarted = false;
		{
			std::lock_guard lock(mutex_);
			if (quiesced_) {
				return false;
			}
			if (!terminal_) {
				terminal_ = true;
				terminal_status_ = timed_out ? flight_error(flight::FlightStatusCode::TimedOut,
				                                            "sidecar execution deadline expired", "DEADLINE_EXCEEDED")
				                             : flight_error(flight::FlightStatusCode::Cancelled,
				                                            "sidecar execution was cancelled", "CANCELLED");
				batch_.reset();
			}
			unstarted = !worker_.joinable();
		}
		execution_->cancel();
		connection_->Interrupt();
		(void)evidence_->finish(sirius::execution_outcome::CANCELLED);
		if (unstarted) {
			std::lock_guard lock(mutex_);
			quiesced_ = true;
			if (!terminal_notified_) {
				terminal_notified_ = true;
				notify_terminal = true;
			}
		}
		condition_.notify_all();
		if (notify_terminal) {
			on_terminal_(ticket_);
		}
		return true;
	}

	bool cancel_and_join(bool timed_out, const std::function<bool()> &stop_waiting = {}) {
		(void)cancel(timed_out);
		const auto deadline =
		    std::chrono::system_clock::time_point(std::chrono::milliseconds(request_.deadline_unix_ms));
		{
			std::unique_lock lock(mutex_);
			while (!quiesced_) {
				lock.unlock();
				const bool stopped = stop_waiting && stop_waiting();
				lock.lock();
				if (stopped) {
					return false;
				}
				const auto now = std::chrono::system_clock::now();
				if (now >= deadline) {
					return false;
				}
				condition_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
			}
		}
		join();
		return true;
	}

	void join() noexcept {
		std::lock_guard worker_lock(worker_mutex_);
		if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
			worker_.join();
		}
	}

private:
	sirius::offload::chunk_action consume(const duckdb::DataChunk &chunk) {
		if (now_unix_ms() >= request_.deadline_unix_ms) {
			cancel(true);
			return sirius::offload::chunk_action::CANCEL;
		}
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [&] { return !batch_ || terminal_; });
			if (terminal_) {
				return sirius::offload::chunk_action::CANCEL;
			}
		}
		auto converted = convert_chunk(chunk, *connection_->context, schema_);
		if (batch_bytes(*converted) > request_.max_batch_bytes) {
			throw std::runtime_error("Arrow result batch exceeds negotiated max_batch_bytes");
		}

		std::unique_lock lock(mutex_);
		if (terminal_) {
			return sirius::offload::chunk_action::CANCEL;
		}
		batch_ = std::move(converted);
		condition_.notify_all();
		condition_.wait(lock, [&] { return !batch_ || terminal_; });
		return terminal_ ? sirius::offload::chunk_action::CANCEL : sirius::offload::chunk_action::CONTINUE;
	}

	void run() noexcept {
		arrow::Status status = arrow::Status::OK();
		sirius::execution_outcome outcome = sirius::execution_outcome::SUCCEEDED;
		try {
			execution_->run([this](const duckdb::DataChunk &chunk) { return consume(chunk); });
		} catch (const sirius::offload::substrait_execution_error &error) {
			status = substrait_error(error);
			outcome = error.code() == sirius::offload::substrait_error_code::CANCELLED
			              ? sirius::execution_outcome::CANCELLED
			              : sirius::execution_outcome::FAILED;
		} catch (const std::exception &error) {
			status = flight_error(flight::FlightStatusCode::Internal,
			                      std::string("sidecar result streaming failed: ") + error.what(), "EXECUTION_FAILED");
			outcome = sirius::execution_outcome::FAILED;
		} catch (...) {
			status =
			    flight_error(flight::FlightStatusCode::Internal, "sidecar result streaming failed", "EXECUTION_FAILED");
			outcome = sirius::execution_outcome::FAILED;
		}
		finish(std::move(status), outcome);
	}

	void finish(arrow::Status status, sirius::execution_outcome outcome) noexcept {
		bool notify_terminal = false;
		{
			std::lock_guard lock(mutex_);
			if (!terminal_) {
				terminal_ = true;
				terminal_status_ = std::move(status);
			}
			quiesced_ = true;
			if (!terminal_notified_) {
				terminal_notified_ = true;
				notify_terminal = true;
			}
		}
		(void)evidence_->finish(outcome);
		condition_.notify_all();
		if (notify_terminal) {
			on_terminal_(ticket_);
		}
	}

	const runtime_config &config_;
	execute_request request_;
	std::string ticket_;
	terminal_callback on_terminal_;

	// Destruction is reverse declaration order: execution (and its resolution
	// tokens) is destroyed before the connection used by token cleanup.
	std::unique_ptr<duckdb::Connection> connection_;
	std::shared_ptr<sirius::execution_evidence> evidence_;
	std::unique_ptr<sirius::offload::substrait_execution> execution_;
	std::shared_ptr<arrow::Schema> schema_;

	std::mutex mutex_;
	std::condition_variable condition_;
	std::shared_ptr<arrow::RecordBatch> batch_;
	arrow::Status terminal_status_ = arrow::Status::OK();
	bool claimed_ = false;
	bool terminal_ = false;
	bool quiesced_ = false;
	bool terminal_notified_ = false;
	// Multiple CancelExecution handlers may observe the same entry before the
	// terminal callback removes it from the registry. std::thread::join is not
	// safe to call concurrently, so serialize the one successful join.
	std::mutex worker_mutex_;
	std::thread worker_;
};

class entry_reader final : public arrow::RecordBatchReader {
public:
	entry_reader(std::shared_ptr<execution_entry> entry, const flight::ServerCallContext &context)
	    : entry_(std::move(entry)), context_(context) {
		entry_->start();
	}

	~entry_reader() override {
		(void)Close();
	}

	std::shared_ptr<arrow::Schema> schema() const override {
		return entry_->schema();
	}
	arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch> *batch) override {
		return entry_->read_next(context_, batch);
	}
	arrow::Status Close() override {
		if (!closed_.exchange(true)) {
			entry_->cancel(false);
		}
		return arrow::Status::OK();
	}

private:
	std::shared_ptr<execution_entry> entry_;
	const flight::ServerCallContext &context_;
	std::atomic<bool> closed_ {false};
};

class ticket_registry final {
public:
	ticket_registry(duckdb::DatabaseInstance &database, const runtime_config &config)
	    : database_(database), config_(config), reaper_([this] { reap(); }) {
	}

	~ticket_registry() {
		shutdown();
	}

	arrow::Result<std::shared_ptr<execution_entry>> prepare(execute_request request) {
		std::shared_ptr<execution_entry> existing;
		{
			std::unique_lock lock(mutex_);
			while (true) {
				if (stopped_) {
					return flight_error(flight::FlightStatusCode::Unavailable, "sidecar is stopping");
				}
				const auto idempotency = idempotency_.find(request.idempotency_key);
				if (idempotency == idempotency_.end()) {
					if (entries_.size() + reserved_ >= config_.max_active_tickets) {
						return flight_error(flight::FlightStatusCode::Unavailable,
						                    "sidecar active ticket limit reached", "RESOURCE_EXHAUSTED");
					}
					++reserved_;
					idempotency_.emplace(request.idempotency_key,
					                     idempotency_record {.fingerprint = request.fingerprint,
					                                         .ticket = {},
					                                         .deadline_unix_ms = request.deadline_unix_ms,
					                                         .preparing = true});
					break;
				}
				if (idempotency->second.fingerprint != request.fingerprint) {
					return flight_error(flight::FlightStatusCode::Failed,
					                    "idempotency key was reused for a different request", "IDEMPOTENCY_CONFLICT");
				}
				if (idempotency->second.preparing) {
					const auto deadline = std::chrono::system_clock::time_point(
					    std::chrono::milliseconds(idempotency->second.deadline_unix_ms));
					if (state_changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
						return flight_error(flight::FlightStatusCode::TimedOut,
						                    "idempotent request did not finish preparing "
						                    "before its deadline",
						                    "IDEMPOTENCY_IN_PROGRESS");
					}
					continue;
				}
				if (idempotency->second.cancel_requested) {
					return flight_error(flight::FlightStatusCode::Failed, "idempotent request is already terminal",
					                    "IDEMPOTENCY_TERMINAL");
				}
				const auto found = entries_.find(idempotency->second.ticket);
				if (found == entries_.end()) {
					return flight_error(flight::FlightStatusCode::Internal,
					                    "idempotent request lost its prepared ticket", "IDEMPOTENCY_STATE_INVALID");
				}
				existing = found->second;
				break;
			}
		}
		if (existing) {
			if (!existing->replayable()) {
				return flight_error(flight::FlightStatusCode::Failed, "idempotent request ticket was already claimed",
				                    "IDEMPOTENCY_ALREADY_CLAIMED");
			}
			return existing;
		}

		std::shared_ptr<execution_entry> entry;
		const auto idempotency_key = request.idempotency_key;
		try {
			std::string ticket;
			do {
				ticket = random_ticket();
			} while (contains(ticket));
			entry = std::make_shared<execution_entry>(
			    database_, config_, std::move(request), ticket,
			    [this](const std::string &completed_ticket) { remove(completed_ticket); });
		} catch (...) {
			release_reservation(idempotency_key);
			throw;
		}

		bool stopping = false;
		bool cancel_requested = false;
		{
			std::lock_guard lock(mutex_);
			--reserved_;
			if (stopped_) {
				stopping = true;
				idempotency_.erase(idempotency_key);
			} else {
				entries_.emplace(entry->ticket(), entry);
				auto &idempotency = idempotency_.at(idempotency_key);
				idempotency.ticket = entry->ticket();
				idempotency.preparing = false;
				cancel_requested = idempotency.cancel_requested;
			}
		}
		state_changed_.notify_all();
		// cancel() calls the registry's terminal callback, so it must never run
		// while mutex_ is held.
		if (stopping) {
			entry->cancel(false);
			return flight_error(flight::FlightStatusCode::Unavailable, "sidecar is stopping");
		}
		if (cancel_requested) {
			(void)entry->cancel_and_join(false);
			return flight_error(flight::FlightStatusCode::Cancelled, "idempotent request was cancelled while preparing",
			                    "CANCELLED");
		}
		return entry;
	}

	std::shared_ptr<execution_entry> claim(const std::string &ticket) {
		std::shared_ptr<execution_entry> result;
		{
			std::lock_guard lock(mutex_);
			const auto found = entries_.find(ticket);
			if (found == entries_.end()) {
				return nullptr;
			}
			result = found->second;
		}
		return result->claim() ? result : nullptr;
	}

	enum class cancel_result : std::uint8_t { NOT_FOUND = 0, QUIESCED, DEADLINE_EXCEEDED };

	cancel_result cancel_and_join(const std::string &ticket, const std::function<bool()> &stop_waiting = {}) {
		std::shared_ptr<execution_entry> entry;
		{
			std::lock_guard lock(mutex_);
			const auto found = entries_.find(ticket);
			if (found == entries_.end()) {
				return cancel_result::NOT_FOUND;
			}
			entry = found->second;
		}
		return entry->cancel_and_join(false, stop_waiting) ? cancel_result::QUIESCED : cancel_result::DEADLINE_EXCEEDED;
	}

	cancel_result cancel_and_join_idempotency(const std::string &idempotency_key,
	                                          const std::function<bool()> &stop_waiting = {}) {
		std::shared_ptr<execution_entry> entry;
		{
			std::unique_lock lock(mutex_);
			while (true) {
				if (stopped_) {
					return cancel_result::NOT_FOUND;
				}
				const auto found = idempotency_.find(idempotency_key);
				if (found == idempotency_.end()) {
					return cancel_result::NOT_FOUND;
				}
				found->second.cancel_requested = true;
				if (!found->second.preparing) {
					const auto ticket = entries_.find(found->second.ticket);
					if (ticket == entries_.end()) {
						return cancel_result::NOT_FOUND;
					}
					entry = ticket->second;
					break;
				}
				const auto deadline =
				    std::chrono::system_clock::time_point(std::chrono::milliseconds(found->second.deadline_unix_ms));
				lock.unlock();
				const bool stopped_waiting = stop_waiting && stop_waiting();
				lock.lock();
				if (stopped_waiting) {
					return cancel_result::DEADLINE_EXCEEDED;
				}
				const auto now = std::chrono::system_clock::now();
				if (now >= deadline) {
					return cancel_result::DEADLINE_EXCEEDED;
				}
				state_changed_.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
			}
		}
		return entry->cancel_and_join(false, stop_waiting) ? cancel_result::QUIESCED : cancel_result::DEADLINE_EXCEEDED;
	}

	void shutdown() noexcept {
		std::vector<std::shared_ptr<execution_entry>> entries;
		{
			std::lock_guard lock(mutex_);
			if (stopped_) {
				return;
			}
			stopped_ = true;
			for (const auto &[_, entry] : entries_) {
				entries.push_back(entry);
			}
		}
		wake_.notify_all();
		state_changed_.notify_all();
		if (reaper_.joinable()) {
			reaper_.join();
		}
		for (const auto &entry : entries) {
			entry->cancel(false);
		}
		for (const auto &entry : entries) {
			entry->join();
		}
		std::lock_guard lock(mutex_);
		entries_.clear();
		idempotency_.clear();
		state_changed_.notify_all();
	}

private:
	bool contains(const std::string &ticket) {
		std::lock_guard lock(mutex_);
		return entries_.contains(ticket);
	}

	void release_reservation(const std::string &idempotency_key) {
		std::lock_guard lock(mutex_);
		--reserved_;
		idempotency_.erase(idempotency_key);
		state_changed_.notify_all();
	}

	void remove(const std::string &ticket) {
		std::lock_guard lock(mutex_);
		const auto found = entries_.find(ticket);
		if (found != entries_.end()) {
			idempotency_.erase(found->second->idempotency_key());
			entries_.erase(found);
		}
		state_changed_.notify_all();
	}

	void reap() noexcept {
		std::unique_lock lock(mutex_);
		while (!stopped_) {
			wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopped_; });
			if (stopped_) {
				break;
			}
			const auto now = now_unix_ms();
			std::vector<std::shared_ptr<execution_entry>> expired;
			for (const auto &[_, entry] : entries_) {
				if (entry->deadline_unix_ms() <= now) {
					expired.push_back(entry);
				}
			}
			lock.unlock();
			for (const auto &entry : expired) {
				entry->cancel(true);
			}
			lock.lock();
		}
	}

	duckdb::DatabaseInstance &database_;
	const runtime_config &config_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable state_changed_;
	std::unordered_map<std::string, std::shared_ptr<execution_entry>> entries_;
	struct idempotency_record {
		std::string fingerprint;
		std::string ticket;
		std::uint64_t deadline_unix_ms = 0;
		bool preparing = false;
		bool cancel_requested = false;
	};
	std::unordered_map<std::string, idempotency_record> idempotency_;
	std::size_t reserved_ = 0;
	bool stopped_ = false;
	std::thread reaper_;
};

class sidecar_flight_server final : public flight::FlightServerBase {
public:
	sidecar_flight_server(duckdb::DatabaseInstance &database, const runtime_config &config)
	    : config_(config), registry_(database, config) {
	}

	void stop_registry() noexcept {
		registry_.shutdown();
	}

	arrow::Status GetFlightInfo(const flight::ServerCallContext &context, const flight::FlightDescriptor &descriptor,
	                            std::unique_ptr<flight::FlightInfo> *info) override {
		if (descriptor.type != flight::FlightDescriptor::CMD) {
			return arrow::Status::Invalid("ExecuteSubstrait requires a command descriptor");
		}
		try {
			auto request = parse_execute_request(descriptor.cmd);
			if (request.idempotency_key != execution_idempotency_key(request.account_id, request.query_id)) {
				return flight_error(flight::FlightStatusCode::Unauthorized,
				                    "ExecuteSubstrait idempotency key does not match its identity",
				                    "AUTHENTICATION_FAILED");
			}
			request.fingerprint = sha256_bytes(descriptor.cmd);
			const auto now = now_unix_ms();
			if (request.protocol_version != k_protocol_version || request.substrait_version != k_substrait_version) {
				return flight_error(flight::FlightStatusCode::Failed,
				                    "unsupported sidecar or Substrait protocol version", "UNSUPPORTED_VERSION");
			}
			if (request.capability_hash != capability_hash()) {
				return flight_error(flight::FlightStatusCode::Failed, "sidecar capability hash mismatch",
				                    "CAPABILITY_MISMATCH");
			}
			if (request.max_batch_bytes == 0 || request.max_batch_bytes > config_.max_batch_bytes) {
				return arrow::Status::Invalid("max_batch_bytes exceeds the sidecar limit");
			}
			if (request.deadline_unix_ms <= now) {
				return flight_error(flight::FlightStatusCode::TimedOut, "execution deadline already expired");
			}
			const auto maximum_deadline = now + config_.ticket_ttl_ms;
			if (request.deadline_unix_ms > maximum_deadline) {
				request.deadline_unix_ms = maximum_deadline;
			}

			auto prepared = registry_.prepare(std::move(request));
			if (!prepared.ok()) {
				return prepared.status();
			}
			auto entry = std::move(prepared).ValueOrDie();
			if (context.is_cancelled()) {
				entry->cancel(false);
				return flight_error(flight::FlightStatusCode::Cancelled, "request cancelled during schema delivery");
			}

			std::vector<flight::FlightEndpoint> endpoints;
			endpoints.emplace_back(flight::Ticket(entry->ticket()), std::vector<flight::Location> {}, std::nullopt,
			                       std::string {});
			auto serialized_schema = arrow::ipc::SerializeSchema(*entry->schema(), arrow::system_memory_pool());
			if (!serialized_schema.ok()) {
				entry->cancel(false);
				return serialized_schema.status();
			}
			flight::FlightInfo::Data data {std::move(serialized_schema).ValueOrDie()->ToString(),
			                               descriptor,
			                               std::move(endpoints),
			                               -1,
			                               -1,
			                               false,
			                               capability_hash()};
			*info = std::make_unique<flight::FlightInfo>(std::move(data));
			return arrow::Status::OK();
		} catch (const sirius::offload::substrait_execution_error &error) {
			return substrait_error(error);
		} catch (const std::invalid_argument &error) {
			return arrow::Status::Invalid(error.what());
		} catch (const std::exception &error) {
			return flight_error(flight::FlightStatusCode::Internal,
			                    std::string("cannot prepare sidecar execution: ") + error.what());
		}
	}

	arrow::Status DoGet(const flight::ServerCallContext &context, const flight::Ticket &request,
	                    std::unique_ptr<flight::FlightDataStream> *stream) override {
		auto entry = registry_.claim(request.ticket);
		if (!entry) {
			return arrow::Status::KeyError("unknown, expired, or already claimed ticket");
		}
		auto reader = std::make_shared<entry_reader>(std::move(entry), context);
		*stream = std::make_unique<flight::RecordBatchStream>(reader);
		return arrow::Status::OK();
	}

	arrow::Status ListActions(const flight::ServerCallContext &, std::vector<flight::ActionType> *actions) override {
		actions->emplace_back("GetCapabilities", "Return the canonical sidecar capability document");
		actions->emplace_back("CancelExecution", "Cancel by opaque Flight ticket or request idempotency key");
		return arrow::Status::OK();
	}

	arrow::Status DoAction(const flight::ServerCallContext &context, const flight::Action &action,
	                       std::unique_ptr<flight::ResultStream> *result) override {
		std::vector<flight::Result> results;
		if (action.type == "GetCapabilities") {
			if (action.body && action.body->size() != 0) {
				return arrow::Status::Invalid("GetCapabilities body must be empty");
			}
			results.emplace_back(arrow::Buffer::FromString(std::string(capability_document())));
		} else if (action.type == "CancelExecution") {
			if (!action.body) {
				return arrow::Status::Invalid("CancelExecution body is required");
			}
			cancel_request request;
			try {
				request = parse_cancel_request(
				    std::string_view(reinterpret_cast<const char *>(action.body->data()), action.body->size()));
			} catch (const std::invalid_argument &error) {
				return arrow::Status::Invalid(error.what());
			}
			const auto stop_waiting = [&context] { return context.is_cancelled(); };
			const auto cancelled = request.ticket.empty()
			                           ? registry_.cancel_and_join_idempotency(request.idempotency_key, stop_waiting)
			                           : registry_.cancel_and_join(request.ticket, stop_waiting);
			if (cancelled == ticket_registry::cancel_result::DEADLINE_EXCEEDED) {
				return flight_error(flight::FlightStatusCode::TimedOut,
				                    "sidecar execution did not quiesce before its deadline", "CANCEL_NOT_QUIESCED");
			}
			results.emplace_back(arrow::Buffer::FromString(
			    cancelled == ticket_registry::cancel_result::QUIESCED ? "quiesced" : "not-found"));
		} else {
			return arrow::Status::NotImplemented("unknown sidecar action: ", action.type);
		}
		*result = std::make_unique<flight::SimpleResultStream>(std::move(results));
		return arrow::Status::OK();
	}

private:
	const runtime_config &config_;
	ticket_registry registry_;
};

} // namespace

class flight_runtime::impl {
public:
	impl(duckdb::DatabaseInstance &database, runtime_config config)
	    : config(std::move(config)), server(database, this->config) {
	}

	runtime_config config;
	sidecar_flight_server server;
	std::thread server_thread;
	std::atomic<bool> started {false};
};

flight_runtime::flight_runtime(duckdb::DatabaseInstance &database, runtime_config config)
    : impl_(std::make_unique<impl>(database, std::move(config))) {
}

flight_runtime::~flight_runtime() noexcept {
	stop();
}

void flight_runtime::start() {
	// Arrow's conda jemalloc backend conflicts with DuckDB/Sirius allocator
	// ownership in this process. Install the process-wide Arrow backend before
	// any Flight worker can initialize the default pool.
	if (::setenv("ARROW_DEFAULT_MEMORY_POOL", "system", 1) != 0) {
		throw std::runtime_error("cannot configure Arrow system memory pool");
	}
	auto location = flight::Location::ForGrpcTls(impl_->config.flight_host, impl_->config.flight_port);
	if (!location.ok()) {
		throw std::runtime_error(location.status().ToString());
	}
	flight::FlightServerOptions options(std::move(location).ValueOrDie());
	options.tls_certificates.push_back(
	    {read_secret_file(impl_->config.flight_cert_path), read_secret_file(impl_->config.flight_key_path)});
	options.verify_client = true;
	options.root_certificates = read_secret_file(impl_->config.flight_client_ca_path);
	const auto initialized = impl_->server.Init(options);
	if (!initialized.ok()) {
		throw std::runtime_error(initialized.ToString());
	}
	impl_->started.store(true);
	impl_->server_thread = std::thread([this] {
		const auto status = impl_->server.Serve();
		(void)status;
		impl_->started.store(false);
	});
}

void flight_runtime::stop() noexcept {
	if (!impl_) {
		return;
	}
	if (impl_->started.exchange(false)) {
		const auto status = impl_->server.Shutdown();
		(void)status;
	}
	if (impl_->server_thread.joinable()) {
		impl_->server_thread.join();
	}
	// No Flight handler can create or claim an entry after Shutdown/Serve join.
	// The registry can now cancel and join every execution worker it owns.
	impl_->server.stop_registry();
}

} // namespace matrixone::sidecar
