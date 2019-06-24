/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
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
#include "statemachine.h"

#include "buckets.h"
#include "connection.h"
#include "connections.h"
#include "cookie.h"
#include "external_auth_manager_thread.h"
#include "front_end_thread.h"
#include "mcaudit.h"
#include "mcbp.h"
#include "mcbp_executors.h"
#include "runtime.h"
#include "sasl_tasks.h"
#include "settings.h"

#include <event2/bufferevent_ssl.h>
#include <logger/logger.h>
#include <mcbp/mcbp.h>
#include <platform/strerror.h>
#include <platform/string_hex.h>
#include <gsl/gsl>

void StateMachine::setCurrentState(State task) {
    // Moving to the same state is legal
    if (task == currentState) {
        return;
    }

    if (connection.isDCP()) {
        /*
         * DCP connections behaves differently than normal
         * connections because they operate in a full duplex mode.
         * New messages may appear from both sides, so we can't block on
         * read from the network / engine
         */

        if (task == State::waiting) {
            task = State::ship_log;
        }
    }

    currentState = task;
}

const char* StateMachine::getStateName(State state) const {
    switch (state) {
    case StateMachine::State::ssl_init:
        return "ssl_init";
    case StateMachine::State::new_cmd:
        return "new_cmd";
    case StateMachine::State::waiting:
        return "waiting";
    case StateMachine::State::read_packet_header:
        return "read_packet_header";
    case StateMachine::State::parse_cmd:
        return "parse_cmd";
    case StateMachine::State::read_packet_body:
        return "read_packet_body";
    case StateMachine::State::closing:
        return "closing";
    case StateMachine::State::pending_close:
        return "pending_close";
    case StateMachine::State::immediate_close:
        return "immediate_close";
    case StateMachine::State::destroyed:
        return "destroyed";
    case StateMachine::State::validate:
        return "validate";
    case StateMachine::State::execute:
        return "execute";
    case StateMachine::State::send_data:
        return "send_data";
    case StateMachine::State::drain_send_buffer:
        return "drain_send_buffer";
    case StateMachine::State::ship_log:
        return "ship_log";
    }

    return "StateMachine::getStateName: Invalid state";
}

bool StateMachine::isIdleState() const {
    switch (currentState) {
    case State::read_packet_header:
    case State::read_packet_body:
    case State::waiting:
    case State::new_cmd:
    case State::ship_log:
    case State::send_data:
    case State::pending_close:
    case State::drain_send_buffer:
    case State::ssl_init:
        return true;
    case State::parse_cmd:
    case State::closing:
    case State::immediate_close:
    case State::destroyed:
    case State::validate:
    case State::execute:
        return false;
    }
    throw std::logic_error("StateMachine::isIdleState: Invalid state");
}

bool StateMachine::execute() {
    switch (currentState) {
    case StateMachine::State::ssl_init:
        return conn_ssl_init();
    case StateMachine::State::new_cmd:
        return conn_new_cmd();
    case StateMachine::State::waiting:
        return conn_waiting();
    case StateMachine::State::read_packet_header:
        return conn_read_packet_header();
    case StateMachine::State::parse_cmd:
        return conn_parse_cmd();
    case StateMachine::State::read_packet_body:
        return conn_read_packet_body();
    case StateMachine::State::closing:
        return conn_closing();
    case StateMachine::State::pending_close:
        return conn_pending_close();
    case StateMachine::State::immediate_close:
        return conn_immediate_close();
    case StateMachine::State::destroyed:
        return conn_destroyed();
    case StateMachine::State::validate:
        return conn_validate();
    case StateMachine::State::execute:
        return conn_execute();
    case StateMachine::State::send_data:
        return conn_send_data();
    case StateMachine::State::drain_send_buffer:
        return conn_drain_send_buffer();
    case StateMachine::State::ship_log:
        return conn_ship_log();
    }
    throw std::invalid_argument("execute(): invalid state");
}

static std::pair<cb::x509::Status, std::string> getCertUserName(Connection& c) {
    auto* ssl_st = bufferevent_openssl_get_ssl(c.bev.get());
    cb::openssl::unique_x509_ptr cert(SSL_get_peer_certificate(ssl_st));
    return settings.lookupUser(cert.get());
}

bool StateMachine::conn_ssl_init() {
    connection.setState(StateMachine::State::new_cmd);
    auto certResult = getCertUserName(connection);
    bool disconnect = false;
    switch (certResult.first) {
    case cb::x509::Status::NoMatch:
    case cb::x509::Status::Error:
        disconnect = true;
        break;
    case cb::x509::Status::NotPresent:
        if (settings.getClientCertMode() == cb::x509::Mode::Mandatory) {
            disconnect = true;
        } else if (is_default_bucket_enabled()) {
            associate_bucket(connection, "default");
        }
        break;
    case cb::x509::Status::Success:
        if (!connection.tryAuthFromSslCert(certResult.second)) {
            disconnect = true;
            // Don't print an error message... already logged
            certResult.second.resize(0);
        }
    }

    if (disconnect) {
        if (certResult.first == cb::x509::Status::NotPresent) {
            audit_auth_failure(connection,
                               "Client did not provide an X.509 certificate");
        } else {
            audit_auth_failure(
                    connection,
                    "Failed to use client provided X.509 certificate");
        }
        connection.setState(StateMachine::State::closing);
        if (!certResult.second.empty()) {
            LOG_WARNING(
                    "{}: conn_ssl_init: disconnection client due to"
                    " error [{}]",
                    connection.getId(),
                    certResult.second);
        }
    }

    return true;
}

/**
 * Ship DCP log to the other end. This state differs with all other states
 * in the way that it support full duplex dialog. We're listening to both read
 * and write events from libevent most of the time. If a read event occurs we
 * switch to the conn_read state to read and execute the input message (that
 * would be an ack message from the other side). If a write event occurs we
 * continue to send DCP log to the other end.
 * @param c the DCP connection to drive
 * @return true if we should continue to process work for this connection, false
 *              if we should start processing events for other connections.
 */
bool StateMachine::conn_ship_log() {
    if (is_bucket_dying(connection)) {
        return true;
    }

    auto& cookie = connection.getCookieObject();
    cookie.setEwouldblock(false);

    switch (connection.tryReadNetwork()) {
    case Connection::TryReadResult::Error:
        // state already set
        return true;

    case Connection::TryReadResult::NoDataReceived:
    case Connection::TryReadResult::DataReceived:
        if (connection.isPacketAvailable()) {
            try_read_mcbp_command(cookie);
            return true;
        }
        break;
    }

    connection.addMsgHdr(true);

    const auto ret = connection.getBucket().getDcpIface()->step(
            static_cast<const void*>(&cookie), &connection);

    switch (connection.remapErrorCode(ret)) {
    case ENGINE_SUCCESS:
        /* The engine got more data it wants to send */
        connection.setState(StateMachine::State::send_data);
        connection.setWriteAndGo(StateMachine::State::ship_log);
        break;
    case ENGINE_EWOULDBLOCK:
        // the engine don't have more data to send at this moment
        return false;
    default:
        LOG_WARNING(
                R"({}: ship_dcp_log - step returned {} - closing connection {})",
                connection.getId(),
                std::to_string(ret),
                connection.getDescription());
        connection.getCookieObject().setEwouldblock(false);
        setCurrentState(State::closing);
    }

    return true;
}

bool StateMachine::conn_waiting() {
    if (is_bucket_dying(connection) || connection.processServerEvents()) {
        return true;
    }

    setCurrentState(State::read_packet_header);
    return true;
}

bool StateMachine::conn_read_packet_header() {
    if (is_bucket_dying(connection) || connection.processServerEvents()) {
        return true;
    }

    auto res = connection.tryReadNetwork();
    switch (res) {
    case Connection::TryReadResult::NoDataReceived:
        setCurrentState(State::waiting);
        return false;
    case Connection::TryReadResult::DataReceived:
        if (connection.read->rsize() >= sizeof(cb::mcbp::Header)) {
            setCurrentState(State::parse_cmd);
        } else {
            setCurrentState(State::waiting);
        }
        return true;
    case Connection::TryReadResult::Error:
        setCurrentState(State::closing);
        return true;
    }

    throw std::logic_error(
            "conn_read_packet_header: Invalid value returned from "
            "tryReadNetwork");
}

bool StateMachine::conn_parse_cmd() {
    // Parse the data in the input pipe and prepare the cookie for execution.
    // If all data is available we'll move over to the execution phase,
    // otherwise we'll wait for the data to arrive
    try_read_mcbp_command(connection.getCookieObject());
    return true;
}

bool StateMachine::conn_new_cmd() {
    if (is_bucket_dying(connection)) {
        return true;
    }

    if (!connection.write->empty()) {
        LOG_WARNING("{}: Expected write buffer to be empty.. It's not! ({})",
                    connection.getId(),
                    connection.write->rsize());
    }

    /*
     * In order to ensure that all clients will be served each
     * connection will only process a certain number of operations
     * before they will back off.
     *
     * Trond Norbye 20171004
     * @todo I've temorarily disabled the fair sharing while moving
     *       over to bufferevents. It'll reappear once we've ironed
     *       out all of the corner cases in the code
     *       (note that this is less of an issue in couchbase than
     *       with memcached as we're more likely to hit an item
     *       which would cause us to block)
     */
    connection.getCookieObject().reset();
    connection.shrinkBuffers();
    if (connection.read->rsize() >= sizeof(cb::mcbp::Request)) {
        setCurrentState(State::parse_cmd);
    } else if (connection.isSslEnabled()) {
        setCurrentState(State::read_packet_header);
    } else {
        setCurrentState(State::waiting);
    }

    return true;
}

bool StateMachine::conn_validate() {
    static McbpValidator packetValidator;

    if (is_bucket_dying(connection)) {
        return true;
    }

    auto& cookie = connection.getCookieObject();
    const auto& header = cookie.getHeader();
    // We validated the basics of the header in try_read_mcbp_command
    // (we needed that in order to know if we could use the length field...

    if (header.isRequest()) {
        const auto& request = header.getRequest();
        if (cb::mcbp::is_client_magic(request.getMagic())) {
            auto opcode = request.getClientOpcode();
            if (!cb::mcbp::is_valid_opcode(opcode)) {
                // We don't know about this command so we can stop
                // processing it. We know that the header adds
                cookie.sendResponse(cb::mcbp::Status::UnknownCommand);
                return true;
            }

            auto result = packetValidator.validate(opcode, cookie);
            if (result != cb::mcbp::Status::Success) {
                LOG_WARNING(
                        R"({}: Invalid format specified for "{}" - Status: "{}" - Closing connection. Packet:[{}] Reason:"{}")",
                        connection.getId(),
                        to_string(opcode),
                        to_string(result),
                        request.toJSON().dump(),
                        cookie.getErrorContext());
                audit_invalid_packet(cookie.getConnection(),
                                     cookie.getPacket());
                cookie.sendResponse(result);
                // sendResponse sets the write and go to continue
                // execute the next command. Instead we want to
                // close the connection. Override the write and go setting
                connection.setWriteAndGo(StateMachine::State::closing);
                return true;
            }
        } else {
            // We should not be receiving a server command.
            // Audit and log
            audit_invalid_packet(connection, cookie.getPacket());
            LOG_WARNING("{}: Received a server command. Closing connection",
                        connection.getId());
            setCurrentState(State::closing);
            return true;
        }
    } // We don't currently have any validators for response packets

    setCurrentState(State::execute);
    return true;
}

bool StateMachine::conn_execute() {
    if (is_bucket_dying(connection)) {
        return true;
    }

    auto& cookie = connection.getCookieObject();
    cookie.setEwouldblock(false);
    connection.enableReadEvent();

    if (!cookie.execute()) {
        connection.disableReadEvent();
        return false;
    }

    // We've executed the packet, and given that we're not blocking we
    // we should move over to the next state. Just do a sanity check
    // for that.
    if (currentState == StateMachine::State::execute) {
        throw std::logic_error(
                "conn_execute: Should leave conn_execute for !EWOULDBLOCK");
    }

    mcbp_collect_timings(cookie);

    // Consume the packet we just executed from the input buffer
    connection.read->consume([&cookie](
                                     cb::const_byte_buffer buffer) -> ssize_t {
        size_t size = cookie.getPacket(Cookie::PacketContent::Full).size();
        if (size > buffer.size()) {
            throw std::logic_error(
                    "conn_execute: Not enough data in input buffer");
        }
        return gsl::narrow<ssize_t>(size);
    });
    // We've cleared the memory for this packet so we need to mark it
    // as cleared in the cookie to avoid having it dumped in toJSON and
    // using freed memory. We cannot call reset on the cookie as we
    // want to preserve the error context and id.
    cookie.clearPacket();
    return true;
}

bool StateMachine::conn_read_packet_body() {
    if (is_bucket_dying(connection)) {
        return true;
    }

    switch (connection.tryReadNetwork()) {
    case Connection::TryReadResult::Error:
        // state already set
        return true;

    case Connection::TryReadResult::DataReceived:
        if (connection.isPacketAvailable()) {
            auto& cookie = connection.getCookieObject();
            auto input = connection.read->rdata();
            const auto* req =
                    reinterpret_cast<const cb::mcbp::Request*>(input.data());
            cookie.setPacket(Cookie::PacketContent::Full,
                             cb::const_byte_buffer{input.data(),
                                                   sizeof(cb::mcbp::Request) +
                                                           req->getBodylen()});
            setCurrentState(State::validate);
            return true;
        }
        // fallthrough
    case Connection::TryReadResult::NoDataReceived:
        return false;
    }

    // Notreached
    throw std::logic_error(
            "conn_read_packet_body: tryReadNetwork returned invalid value");
}

bool StateMachine::conn_send_data() {
    bool ret = true;

    switch (connection.transmit()) {
    case Connection::TransmitResult::Complete:
        // Release all allocated resources
        connection.releaseTempAlloc();
        connection.releaseReservedItems();
        setCurrentState(State::drain_send_buffer);
        return true;

    case Connection::TransmitResult::Incomplete:
        LOG_DEBUG("{} - Incomplete transfer. Will retry", connection.getId());
        break;

    case Connection::TransmitResult::HardError:
        LOG_INFO("{} - Hard error, closing connection", connection.getId());
        break;

    case Connection::TransmitResult::SoftError:
        ret = false;
        break;
    }

    if (is_bucket_dying(connection)) {
        return true;
    }

    return ret;
}

bool StateMachine::conn_drain_send_buffer() {
    if (connection.havePendingData()) {
        return false;
    }

    // We're done sending the response to the client. Enter the next
    // state in the state machine
    connection.setState(connection.getWriteAndGo());
    return true;
}

bool StateMachine::conn_immediate_close() {
    disassociate_bucket(connection);

    // Do the final cleanup of the connection:
    auto& thread = connection.getThread();
    thread.notification.remove(&connection);
    // remove from pending-io list
    std::lock_guard<std::mutex> lock(thread.pending_io.mutex);
    thread.pending_io.map.erase(&connection);

    connection.bev.reset();

    // Set the connection to the sentinal state destroyed and return
    // false to break out of the event loop (and have the the framework
    // delete the connection object).
    setCurrentState(State::destroyed);

    return false;
}

bool StateMachine::conn_pending_close() {
    return connection.close();
}

bool StateMachine::conn_closing() {
    externalAuthManager->remove(connection);
    return connection.close();
}

/** sentinal state used to represent a 'destroyed' connection which will
 *  actually be freed at the end of the event loop. Always returns false.
 */
bool StateMachine::conn_destroyed() {
    return false;
}
