/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#pragma once

#include "dynamic_buffer.h"
#include "tracing/tracer.h"

#include <mcbp/protocol/datatype.h>
#include <mcbp/protocol/status.h>
#include <memcached/dockey.h>
#include <memcached/engine_error.h>
#include <nlohmann/json.hpp>
#include <platform/sized_buffer.h>
#include <chrono>

// Forward decls
class Connection;
class CommandContext;
struct CookieTraceContext;
namespace cb {
namespace mcbp {
class Header;
class Request;
class Response;
} // namespace mcbp
} // namespace cb

/**
 * The Cookie class represents the cookie passed from the memcached core
 * down through the engine interface to the engine.
 *
 * A cookie represents a single command context, and contains the packet
 * it is about to execute.
 *
 * By passing a common class as the cookie our notification model may
 * know what the argument is and provide it's own logic depending on
 * which field is set
 */
class Cookie {
public:
    explicit Cookie(Connection& conn);

    /**
     * Initialize this cookie.
     *
     * At some point we'll refactor this into being the constructor
     * for the cookie. Currently we create a single cookie object per
     * connection which handle all of the commands (and we'll call the
     * initialize method every time we're starting on a new one), but
     * in the future we'll have multiple commands per connection and
     * this method should be the constructor).
     *
     * @param packet the entire packet
     * @param tracing_enabled if tracing is enabled for this request
     */
    void initialize(const cb::mcbp::Header& packet, bool tracing_enabled);

    /// Is this object initialized or not..
    bool empty() const {
        return packet == nullptr;
    }

    /**
     * Validates the packet content, and (possibly) set the error
     * state and reason.
     *
     * @return Success if the packet was correctly encoded
     * @throw std::runtime_error if an unsupported packet is encountered
     */
    cb::mcbp::Status validate();

    /**
     * Reset the Cookie object to allow it to be reused in the same
     * context as the last time.
     */
    void reset();

    /**
     * Get a representation of the object in JSON
     */
    nlohmann::json toJSON() const;

    /**
     * Get the unique event identifier created for this command. It should
     * be included in all log messages related to a given request, and
     * returned in the response sent back to the client.
     *
     * @return A "random" UUID
     */
    const std::string& getEventId() const;

    void setEventId(std::string uuid) {
        event_id = std::move(uuid);
    }

    /**
     * Does this cookie contain a UUID to be inserted into the error
     * message to be sent back to the client.
     */
    bool hasEventId() const {
        return !event_id.empty();
    }

    /**
     * Add a more descriptive error context to response sent back for
     * this command.
     * Note this has no affect for the following response codes.
     *   cb::mcbp::Status::Success
     *   cb::mcbp::Status::SubdocSuccessDeleted
     *   cb::mcbp::Status::SubdocMultiPathFailure
     *   cb::mcbp::Status::Rollback
     *   cb::mcbp::Status::NotMyVbucket
     *
     * @param message a string which will become the value of the "context" key
     *        in the JSON response object
     */
    void setErrorContext(std::string message) {
        error_context = std::move(message);
    }

    /**
     * Add additional arbitrary JSON to the response, this is in addition to
     * any message set via setErrorContext and any id set via setEventId
     *
     * Note this has no affect for the following response codes.
     *   cb::mcbp::Status::Success
     *   cb::mcbp::Status::SubdocSuccessDeleted
     *   cb::mcbp::Status::SubdocMultiPathFailure
     *   cb::mcbp::Status::Rollback
     *   cb::mcbp::Status::NotMyVbucket
     *
     * @param json an object which is appended to the JSON response object.
     */
    void setErrorJsonExtras(const nlohmann::json& json);

    /**
     * Get the context to send back for this command.
     */
    const std::string& getErrorContext() const {
        return error_context;
    }

    /**
     * Return the error "object" to return to the client.
     *
     * @return An empty string if no extended error information is being set
     */
    const std::string& getErrorJson();

    /**
     * Get the connection object the cookie is bound to.
     *
     * A cookie is bound to the conneciton at create time, and will never
     * switch connections
     */
    Connection& getConnection() const {
        return connection;
    }

    /**
     *
     * Clear the dynamic buffer
     */
    void clearDynamicBuffer() {
        dynamicBuffer.clear();
    }

    /**
     * Grow the dynamic buffer to
     */
    bool growDynamicBuffer(size_t needed) {
        return dynamicBuffer.grow(needed);
    }

    DynamicBuffer& getDynamicBuffer() {
        return dynamicBuffer;
    }

    /**
     * Execute the current packet
     *
     * Given that the method calls down into the engine it may throw a wide
     * range of exceptions, but they should all be based off std::exception
     * which is caught by the state machinery causing the connection to be
     * closed
     *
     * @return true if the command executed completely
     *         false if the command blocked (ewouldblock)
     */
    bool execute();

    /**
     * Set the packet used by this command context.
     *
     *
     * Note that the cookie does not _own_ the actual packet content
     * (unless copy is set to true), as we might not want to perform
     * an extra memory copy from the underlying event framework
     * into the cookie and then again into the underlying engine.
     *
     * The initial prototype of unordered execution will however
     * do the copy to simplify the state machinery logic.
     *
     * @param header the header and the full packet to use
     * @param copy Set to true if the cookie should create a copy
     *             of the data (to be returned from the getPackage)
     *
     * @throw std::invalid_argument if buffer size < a request
     * @throw std::logic_error if copy is requested and content
     *                         isn't the full packet
     * @throw std::bad_alloc if copy is set to true and we fail to
     *                       allocate a backing store.
     */
    void setPacket(const cb::mcbp::Header& header, bool copy = false);

    /**
     * Get the packet for this command / response packet
     *
     * @return the byte buffer containing the packet
     * @throws std::logic_error if the packet isn't available
     */
    cb::const_byte_buffer getPacket() const;

    void clearPacket() {
        packet = nullptr;
    }

    /**
     * Preserve the input packet by allocating memory and copy the
     * current packet.
     */
    void preserveRequest() {
        setPacket(getHeader(), true);
    }

    /**
     * Get the packet header for the current packet. The packet header
     * allows for getting the various common fields in a packet (request and
     * response).
     */
    const cb::mcbp::Header& getHeader() const;

    /**
     * Get the packet as a request packet
     *
     * @return the packet if it is a request
     * @throws std::invalid_argument if the packet is of an invalid type
     * @throws std::logic_error if the packet is a response
     */
    const cb::mcbp::Request& getRequest() const;

    /**
     * Get the key from the request
     *
     * @return the key from the request
     * @throws std::invalid_argument if the packet is of an invalid type
     * @throws std::logic_error if the packet is a response
     */
    const DocKey getRequestKey() const;

    /**
     * Get a printable key from the header. Replace all non-printable
     * charachters with '.'
     */
    std::string getPrintableRequestKey() const;

    /**
     * Get the packet as a response packet
     *
     * @return the packet if it is a response, or nullptr if it is a requests
     * @throws std::invalid_argument if the packet is of an invalid type
     */
    const cb::mcbp::Response& getResponse() const;

    /**
     * Log the start of processing a command received from the client in the
     * generic form which (may change over time, but currently it) looks like:
     *
     *     id> COMMAND KEY
     */
    void logCommand() const;

    /**
     * Log the end of processing a command and the result of the command:
     *
     *     id< COMMAND KEY - STATUS
     *
     * @param code The execution result
     */
    void logResponse(ENGINE_ERROR_CODE code) const;

    /**
     * Set the aiostat and return the previous value
     */
    ENGINE_ERROR_CODE swapAiostat(ENGINE_ERROR_CODE value);

    /**
     * Get the current status of the asynchrous IO
     */
    ENGINE_ERROR_CODE getAiostat() const;

    /**
     * Set the status code for the async IO
     */
    void setAiostat(ENGINE_ERROR_CODE aiostat);

    /**
     * Is the current cookie blocked?
     */
    bool isEwouldblock() const {
        return ewouldblock;
    }

    /**
     * Set the ewouldblock status for the cookie
     */
    void setEwouldblock(bool ewouldblock);

    /**
     *
     * @return
     */
    uint64_t getCas() const {
        return cas;
    }

    /**
     * Set the CAS value to inject into the response packet
     */
    void setCas(uint64_t cas) {
        Cookie::cas = cas;
    }

    /**
     * Send the dynamic buffer
     */
    void sendDynamicBuffer();

    /**
     * Sent back the not my vbucket response (piggyback a vbucket
     * map and do deduplication if enabled)
     */
    void sendNotMyVBucket();

    /**
     * Send a response without a message payload back to the client.
     *
     * @param status The status message to fill into the message.
     */
    void sendResponse(cb::mcbp::Status status);

    /**
     * Map the engine error code over to the correct status message
     * and send the appropriate packet back to the client.
     */
    void sendResponse(cb::engine_errc code);

    /**
     * Form a response packet and send back to the client
     *
     * Note: we currently _copy_ the content of extras, key and value
     * into the connections write buffer.
     *
     * @param status The status code for the operation
     * @param extras The extras to add to the package
     * @param key The key to add to the package
     * @param value The value to add to the packet
     * @param datatype The datatype to add to the message
     * @param cas the Cas field to insert into the packet
     *
     * @throws std::bad_alloc for memory alloction failures
     * @throws std::runtime_error if unsupported datatypes is being used
     *                            (xattrs or compressed)
     * @throws std::logic_error if the write buffer contains data
     */
    void sendResponse(cb::mcbp::Status status,
                      cb::const_char_buffer extras,
                      cb::const_char_buffer key,
                      cb::const_char_buffer value,
                      cb::mcbp::Datatype datatype,
                      uint64_t cas);

    /**
     * Get the command context stored for this command as
     * the given type or make it if it doesn't exist
     *
     * @tparam ContextType CommandContext type to create
     * @return the context object
     * @throws std::logic_error if the object is the wrong type
     */
    template <typename ContextType, typename... Args>
    ContextType& obtainContext(Args&&... args) {
        auto* context = commandContext.get();
        if (context == nullptr) {
            auto* ret = new ContextType(std::forward<Args>(args)...);
            commandContext.reset(ret);
            return *ret;
        }
        auto* ret = dynamic_cast<ContextType*>(context);
        if (ret == nullptr) {
            throw std::logic_error(std::string("Connection::obtainContext<") +
                                   typeid(ContextType).name() +
                                   ">(): context is not the requested type");
        }
        return *ret;
    }

    CommandContext* getCommandContext() {
        return commandContext.get();
    }

    void setCommandContext(CommandContext* ctx = nullptr);

    /**
     * Log the current connection if its execution time exceeds the
     * threshold for the command
     *
     * @param elapsed the time elapsed while executing the command
     */
    void maybeLogSlowCommand(std::chrono::steady_clock::duration elapsed) const;

    /**
     * Get the start time for this command
     */
    std::chrono::steady_clock::time_point getStart() const {
        return start;
    }

    bool isTracingEnabled() const {
        return enableTracing;
    }

    void setTracingEnabled(bool enable) {
        enableTracing = enable;
    }

    cb::tracing::Tracer& getTracer() {
        return tracer;
    }

    const cb::tracing::Tracer& getTracer() const {
        return tracer;
    }

    uint8_t getRefcount() {
        return refcount;
    }
    void incrementRefcount() {
        if (refcount == 255) {
            throw std::logic_error(
                    "Cookie::incrementRefcount(): refcount will wrap");
        }
        refcount++;
    }
    void decrementRefcount() {
        if (refcount == 0) {
            throw std::logic_error(
                    "Cookie::decrementRefcount(): refcount will wrap");
        }
        refcount--;
    }

    void* getEngineStorage() const {
        return engine_storage;
    }

    void setEngineStorage(void* engine_storage) {
        Cookie::engine_storage = engine_storage;
    }

    void setOpenTracingContext(cb::const_byte_buffer context);

    /**
     * Is OpenTracing enabled for this cookie or not. By querying the
     * cookie we don't have to read the atomic on/off switch unless
     * people try to set the OpenTracing context in the command.
     */
    bool isOpenTracingEnabled() const {
        return !openTracingContext.empty();
    }

    /**
     * Extract the trace context.
     * This method moves the tracer, OpenTracing context into
     * a newly created TraceContext object which may be passed to the
     * OpenTracing module and handled asynchronously
     */
    CookieTraceContext extractTraceContext();

    bool isValidated() const {
        return validated;
    }

    void setReorder() {
        reorder = true;
    }

    bool mayReorder() const {
        return reorder;
    }

protected:
    bool enableTracing = false;
    cb::tracing::Tracer tracer;
    bool validated = false;

    bool reorder = false;

    /// The tracing context provided by the client to use as the
    /// parent span
    std::string openTracingContext;

    /**
     * The connection object this cookie is bound to
     */
    Connection& connection;

    mutable std::string event_id;
    std::string error_context;
    nlohmann::json error_extra_json;

    /**
     * A member variable to keep the data around until it's been safely
     * transferred to the client.
     */
    std::string json_message;

    /**
     * The input packet used in this command context
     */
    const cb::mcbp::Header* packet = nullptr;

    /**
     * The backing store of the received packet if the cookie owns
     * the data (created by copying the input data)
     */
    std::unique_ptr<uint8_t[]> frame_copy;

    /**
     * The dynamic buffer is used to format output packets to be sent on
     * the wire.
     */
    DynamicBuffer dynamicBuffer;

    /** The cas to return back to the client */
    uint64_t cas = 0;

    /**
     * The high resolution timer value for when we started executing the
     * current command.
     */
    std::chrono::steady_clock::time_point start;

    /**
     *  command-specific context - for use by command executors to maintain
     *  additional state while executing a command. For example
     *  a command may want to maintain some temporary state between retries
     *  due to engine returning EWOULDBLOCK.
     *
     *  Between each command this is deleted and reset to nullptr.
     */
    std::unique_ptr<CommandContext> commandContext;

    /**
     * Pointer to engine-specific data which the engine has requested the server
     * to persist for the life of the connection.
     * See SERVER_COOKIE_API::{get,store}_engine_specific()
     */
    void* engine_storage{nullptr};

    /**
     * Log a preformatted response text
     *
     * @param reason the text to log
     */
    void logResponse(const char* reason) const;

    /**
     * The status for the async io operation
     */
    ENGINE_ERROR_CODE aiostat = ENGINE_SUCCESS;

    bool ewouldblock = false;

    /// The number of times someone tried to reserve the cookie (to avoid
    /// releasing it while other parties think they reserved the object.
    /// Previously reserve would lock the connection, but with OOO we
    /// might have multiple cookies in flight and needs to be able to
    /// lock them independently
    uint8_t refcount = 0;
};
