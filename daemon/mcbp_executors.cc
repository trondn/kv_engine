/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
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

#include "mcbp_executors.h"

#include "buckets.h"
#include "config_parse.h"
#include "connections.h"
#include "debug_helpers.h"
#include "enginemap.h"
#include "ioctl.h"
#include "mc_time.h"
#include "mcaudit.h"
#include "mcbp.h"
#include "mcbp_privileges.h"
#include "mcbp_topkeys.h"
#include "mcbp_validators.h"
#include "mcbpdestroybuckettask.h"
#include "memcached.h"
#include "protocol/mcbp/appendprepend_context.h"
#include "protocol/mcbp/arithmetic_context.h"
#include "protocol/mcbp/audit_configure_context.h"
#include "protocol/mcbp/create_remove_bucket_command_context.h"
#include "protocol/mcbp/dcp_deletion.h"
#include "protocol/mcbp/dcp_expiration.h"
#include "protocol/mcbp/dcp_mutation.h"
#include "protocol/mcbp/dcp_system_event_executor.h"
#include "protocol/mcbp/engine_wrapper.h"
#include "protocol/mcbp/executors.h"
#include "protocol/mcbp/flush_command_context.h"
#include "protocol/mcbp/gat_context.h"
#include "protocol/mcbp/get_context.h"
#include "protocol/mcbp/get_locked_context.h"
#include "protocol/mcbp/mutation_context.h"
#include "protocol/mcbp/rbac_reload_command_context.h"
#include "protocol/mcbp/remove_context.h"
#include "protocol/mcbp/sasl_auth_command_context.h"
#include "protocol/mcbp/sasl_refresh_command_context.h"
#include "protocol/mcbp/stats_context.h"
#include "protocol/mcbp/steppable_command_context.h"
#include "protocol/mcbp/unlock_context.h"
#include "protocol/mcbp/utilities.h"
#include "runtime.h"
#include "sasl_tasks.h"
#include "session_cas.h"
#include "settings.h"
#include "subdocument.h"

#include <cctype>
#include <memcached/rbac.h>
#include <platform/cb_malloc.h>
#include <platform/checked_snprintf.h>
#include <platform/compress.h>
#include <snappy-c.h>
#include <utilities/protocol2text.h>

std::array<bool, 0x100>&  topkey_commands = get_mcbp_topkeys();
std::array<mcbp_package_execute, 0x100>& executors = get_mcbp_executors();

static bool authenticated(McbpConnection* c) {
    bool rv;

    switch (c->getCmd()) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS: /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_AUTH:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_SASL_STEP:       /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_VERSION:         /* FALLTHROUGH */
    case PROTOCOL_BINARY_CMD_HELLO:
        rv = true;
        break;
    default:
        rv = c->isAuthenticated();
    }

    if (settings.getVerbose() > 1) {
        LOG_DEBUG(c, "%u: authenticated() in cmd 0x%02x is %s",
                  c->getId(), c->getCmd(), rv ? "true" : "false");
    }

    return rv;
}

static void bin_read_chunk(McbpConnection* c, uint32_t chunk) {
    ptrdiff_t offset;
    c->setRlbytes(chunk);

    /* Ok... do we have room for everything in our buffer? */
    offset =
        c->read.curr + sizeof(protocol_binary_request_header) - c->read.buf;
    if (c->getRlbytes() > c->read.size - offset) {
        size_t nsize = c->read.size;
        size_t size = c->getRlbytes() + sizeof(protocol_binary_request_header);

        while (size > nsize) {
            nsize *= 2;
        }

        if (nsize != c->read.size) {
            char* newm;
            LOG_DEBUG(c, "%u: Need to grow buffer from %lu to %lu",
                      c->getId(), (unsigned long)c->read.size,
                      (unsigned long)nsize);
            newm = reinterpret_cast<char*>(cb_realloc(c->read.buf, nsize));
            if (newm == NULL) {
                LOG_WARNING(c, "%u: Failed to grow buffer.. closing connection",
                            c->getId());
                c->setState(conn_closing);
                return;
            }

            c->read.buf = newm;
            /* rcurr should point to the same offset in the packet */
            c->read.curr =
                c->read.buf + offset - sizeof(protocol_binary_request_header);
            c->read.size = (int)nsize;
        }
        if (c->read.buf != c->read.curr) {
            memmove(c->read.buf, c->read.curr, c->read.bytes);
            c->read.curr = c->read.buf;
            LOG_DEBUG(c, "%u: Repack input buffer", c->getId());
        }
    }

    // The input buffer is big enough to fit the entire packet.
    // Go fetch the rest of the data
    c->setState(conn_read_packet_body);
}

/* Just write an error message and disconnect the client */
static void handle_binary_protocol_error(McbpConnection* c) {
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL);
    LOG_NOTICE(c, "%u: Protocol error (opcode %02x), close connection",
               c->getId(), c->binary_header.request.opcode);
    c->setWriteAndGo(conn_closing);
}

/**
 * Triggers topkeys_update (i.e., increments topkeys stats) if called by a
 * valid operation.
 */
void update_topkeys(const DocKey& key, McbpConnection* c) {

    if (topkey_commands[c->binary_header.request.opcode]) {
        if (all_buckets[c->getBucketIndex()].topkeys != nullptr) {
            all_buckets[c->getBucketIndex()].topkeys->updateKey(key.data(),
                                                                key.size(),
                                                                mc_time_get_current_time());
        }
    }
}

static void process_bin_get(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_get*>(packet);
    c->obtainContext<GetCommandContext>(*c, req).drive();
}

static void get_locked_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_getl*>(packet);
    c->obtainContext<GetLockedCommandContext>(*c, req).drive();
}

static void unlock_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_no_extras*>(packet);
    c->obtainContext<UnlockCommandContext>(*c, req).drive();
}

static void gat_executor(McbpConnection* c, void* packet) {
    if (c->getCmd() == PROTOCOL_BINARY_CMD_GATQ) {
        c->setNoReply(true);
    }
    auto* req = reinterpret_cast<protocol_binary_request_gat*>(packet);
    c->obtainContext<GatCommandContext>(*c, *req).drive();
}


static ENGINE_ERROR_CODE default_unknown_command(
    EXTENSION_BINARY_PROTOCOL_DESCRIPTOR*,
    ENGINE_HANDLE*,
    const void* void_cookie,
    protocol_binary_request_header* request,
    ADD_RESPONSE response) {

    auto* cookie = reinterpret_cast<const Cookie*>(void_cookie);
    if (cookie->connection == nullptr) {
        throw std::logic_error("default_unknown_command: connection can't be null");
    }
    // Using dynamic cast to ensure a coredump when we implement this for
    // Greenstack and fix it
    auto* c = dynamic_cast<McbpConnection*>(cookie->connection);
    return bucket_unknown_command(c, request, response);
}

struct request_lookup {
    EXTENSION_BINARY_PROTOCOL_DESCRIPTOR* descriptor;
    BINARY_COMMAND_CALLBACK callback;
};

static struct request_lookup request_handlers[0x100];

typedef void (* RESPONSE_HANDLER)(McbpConnection*);

/**
 * A map between the response packets op-code and the function to handle
 * the response message.
 */
static std::array<RESPONSE_HANDLER, 0x100> response_handlers;

void setup_mcbp_lookup_cmd(
    EXTENSION_BINARY_PROTOCOL_DESCRIPTOR* descriptor,
    uint8_t cmd,
    BINARY_COMMAND_CALLBACK new_handler) {
    request_handlers[cmd].descriptor = descriptor;
    request_handlers[cmd].callback = new_handler;
}

static void process_bin_unknown_packet(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_header*>(packet);
    ENGINE_ERROR_CODE ret = c->getAiostat();
    c->setAiostat(ENGINE_SUCCESS);
    c->setEwouldblock(false);

    if (ret == ENGINE_SUCCESS) {
        struct request_lookup* rq =
            request_handlers + c->binary_header.request.opcode;
        ret = rq->callback(rq->descriptor,
                           c->getBucketEngineAsV0(), c->getCookie(), req,
                           mcbp_response_handler);
    }


    switch (ret) {
    case ENGINE_SUCCESS: {
        if (c->getDynamicBuffer().getRoot() != nullptr) {
            // We assume that if the underlying engine returns a success then
            // it is sending a success to the client.
            ++c->getBucket().responseCounters[PROTOCOL_BINARY_RESPONSE_SUCCESS];
            mcbp_write_and_free(c, &c->getDynamicBuffer());
        } else {
            c->setState(conn_new_cmd);
        }
        update_topkeys(DocKey(req->bytes + sizeof(c->binary_header.request) +
                                      c->binary_header.request.extlen,
                              c->binary_header.request.keylen,
                              c->getDocNamespace()),
                       c);
        break;
    }
    case ENGINE_EWOULDBLOCK:
        c->setEwouldblock(true);
        break;
    case ENGINE_DISCONNECT:
        c->setState(conn_closing);
        break;
    default:
        /* Release the dynamic buffer.. it may be partial.. */
        c->clearDynamicBuffer();
        mcbp_write_packet(c, engine_error_2_mcbp_protocol_error(ret));
    }
}

/**
 * We received a noop response.. just ignore it
 */
static void process_bin_noop_response(McbpConnection* c) {
    c->setState(conn_new_cmd);
}

/*******************************************************************************
 **                             DCP MESSAGE PRODUCERS                         **
 ******************************************************************************/

static ENGINE_ERROR_CODE add_packet_to_pipe(McbpConnection* c,
                                            cb::const_byte_buffer packet) {
    auto wbuf = c->write->wdata();
    if (wbuf.size() < packet.size()) {
        return ENGINE_E2BIG;
    }

    std::copy(packet.begin(), packet.end(), wbuf.begin());
    c->addIov(wbuf.data(), packet.size());
    c->write->produced(packet.size());
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE dcp_message_get_failover_log(const void* void_cookie,
                                                      uint32_t opaque,
                                                      uint16_t vbucket) {
    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG);

    protocol_binary_request_dcp_get_failover_log packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_stream_req(const void* void_cookie,
                                                uint32_t opaque,
                                                uint16_t vbucket,
                                                uint32_t flags,
                                                uint64_t start_seqno,
                                                uint64_t end_seqno,
                                                uint64_t vbucket_uuid,
                                                uint64_t snap_start_seqno,
                                                uint64_t snap_end_seqno) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_STREAM_REQ);

    protocol_binary_request_dcp_stream_req packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_STREAM_REQ;
    packet.message.header.request.extlen = 48;
    packet.message.header.request.bodylen = htonl(48);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.body.flags = ntohl(flags);
    packet.message.body.start_seqno = ntohll(start_seqno);
    packet.message.body.end_seqno = ntohll(end_seqno);
    packet.message.body.vbucket_uuid = ntohll(vbucket_uuid);
    packet.message.body.snap_start_seqno = ntohll(snap_start_seqno);
    packet.message.body.snap_end_seqno = ntohll(snap_end_seqno);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_add_stream_response(const void* void_cookie,
                                                         uint32_t opaque,
                                                         uint32_t dialogopaque,
                                                         uint8_t status) {
    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_ADD_STREAM);
    protocol_binary_response_dcp_add_stream packet = {};
    packet.message.header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_ADD_STREAM;
    packet.message.header.response.extlen = 4;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = htonl(4);
    packet.message.header.response.opaque = opaque;
    packet.message.body.opaque = ntohl(dialogopaque);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_marker_response(const void* void_cookie,
                                                     uint32_t opaque,
                                                     uint8_t status) {
    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER);
    protocol_binary_response_dcp_snapshot_marker packet = {};
    packet.message.header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    packet.message.header.response.extlen = 0;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = 0;
    packet.message.header.response.opaque = opaque;

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_set_vbucket_state_response(
    const void* void_cookie,
    uint32_t opaque,
    uint8_t status) {

    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE);
    protocol_binary_response_dcp_set_vbucket_state packet = {};
    packet.message.header.response.magic = (uint8_t)PROTOCOL_BINARY_RES;
    packet.message.header.response.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE;
    packet.message.header.response.extlen = 0;
    packet.message.header.response.status = htons(status);
    packet.message.header.response.bodylen = 0;
    packet.message.header.response.opaque = opaque;

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_stream_end(const void* void_cookie,
                                                uint32_t opaque,
                                                uint16_t vbucket,
                                                uint32_t flags) {
    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_STREAM_END);

    protocol_binary_request_dcp_stream_end packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_STREAM_END;
    packet.message.header.request.extlen = 4;
    packet.message.header.request.bodylen = htonl(4);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.body.flags = ntohl(flags);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_marker(const void* void_cookie,
                                            uint32_t opaque,
                                            uint16_t vbucket,
                                            uint64_t start_seqno,
                                            uint64_t end_seqno,
                                            uint32_t flags) {
    auto* c = cookie2mcbp(void_cookie, __func__);

    c->setCmd(PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER);
    protocol_binary_request_dcp_snapshot_marker packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.extlen = 20;
    packet.message.header.request.bodylen = htonl(20);
    packet.message.body.start_seqno = htonll(start_seqno);
    packet.message.body.end_seqno = htonll(end_seqno);
    packet.message.body.flags = htonl(flags);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_flush(const void* void_cookie,
                                           uint32_t opaque,
                                           uint16_t vbucket) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_FLUSH);
    protocol_binary_request_dcp_flush packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_FLUSH;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_set_vbucket_state(const void* void_cookie,
                                                       uint32_t opaque,
                                                       uint16_t vbucket,
                                                       vbucket_state_t state) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE);
    protocol_binary_request_dcp_set_vbucket_state packet = {};

    if (!is_valid_vbucket_state_t(state)) {
        return ENGINE_EINVAL;
    }

    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE;
    packet.message.header.request.extlen = 1;
    packet.message.header.request.bodylen = htonl(1);
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.body.state = uint8_t(state);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_noop(const void* void_cookie,
                                          uint32_t opaque) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_NOOP);
    protocol_binary_request_dcp_noop packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_NOOP;
    packet.message.header.request.opaque = opaque;

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_buffer_acknowledgement(const void* void_cookie,
                                                            uint32_t opaque,
                                                            uint16_t vbucket,
                                                            uint32_t buffer_bytes) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT);
    protocol_binary_request_dcp_buffer_acknowledgement packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT;
    packet.message.header.request.extlen = 4;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.vbucket = htons(vbucket);
    packet.message.header.request.bodylen = ntohl(4);
    packet.message.body.buffer_bytes = ntohl(buffer_bytes);

    return add_packet_to_pipe(c, {packet.bytes, sizeof(packet.bytes)});
}

static ENGINE_ERROR_CODE dcp_message_control(const void* void_cookie,
                                             uint32_t opaque,
                                             const void* key,
                                             uint16_t nkey,
                                             const void* value,
                                             uint32_t nvalue) {
    auto* c = cookie2mcbp(void_cookie, __func__);
    c->setCmd(PROTOCOL_BINARY_CMD_DCP_CONTROL);
    protocol_binary_request_dcp_control packet = {};
    packet.message.header.request.magic = (uint8_t)PROTOCOL_BINARY_REQ;
    packet.message.header.request.opcode = (uint8_t)PROTOCOL_BINARY_CMD_DCP_CONTROL;
    packet.message.header.request.opaque = opaque;
    packet.message.header.request.keylen = ntohs(nkey);
    packet.message.header.request.bodylen = ntohl(nvalue + nkey);

    auto wbuf = c->write->wdata();
    if (wbuf.size() < (sizeof(packet.bytes) + nkey + nvalue)) {
        return ENGINE_E2BIG;
    }

    std::copy(packet.bytes, packet.bytes + sizeof(packet.bytes), wbuf.data());

    std::copy(static_cast<const uint8_t*>(key),
              static_cast<const uint8_t*>(key) + nkey,
              wbuf.data() + sizeof(packet.bytes));

    std::copy(static_cast<const uint8_t*>(value),
              static_cast<const uint8_t*>(value) + nvalue,
              wbuf.data() + sizeof(packet.bytes) + nkey);

    c->addIov(wbuf.data(), sizeof(packet.bytes) + nkey + nvalue);
    c->write->produced(sizeof(packet.bytes) + nkey + nvalue);

    return ENGINE_SUCCESS;
}

void ship_mcbp_dcp_log(McbpConnection* c) {
    static struct dcp_message_producers producers = {
        dcp_message_get_failover_log,
        dcp_message_stream_req,
        dcp_message_add_stream_response,
        dcp_message_marker_response,
        dcp_message_set_vbucket_state_response,
        dcp_message_stream_end,
        dcp_message_marker,
        dcp_message_mutation,
        dcp_message_deletion,
        dcp_message_expiration,
        dcp_message_flush,
        dcp_message_set_vbucket_state,
        dcp_message_noop,
        dcp_message_buffer_acknowledgement,
        dcp_message_control,
        dcp_message_system_event
    };
    ENGINE_ERROR_CODE ret;

    // Begin timing DCP, each dcp callback needs to set the c->cmd for the timing
    // to be recorded.
    c->setStart(gethrtime());

    c->addMsgHdr(true);
    c->setEwouldblock(false);
    ret = c->getBucketEngine()->dcp.step(c->getBucketEngineAsV0(), c->getCookie(),
                                         &producers);
    if (ret == ENGINE_SUCCESS) {
        /* the engine don't have more data to send at this moment */
        c->setEwouldblock(true);
    } else if (ret == ENGINE_WANT_MORE) {
        /* The engine got more data it wants to send */
        ret = ENGINE_SUCCESS;
        c->setState(conn_send_data);
        c->setWriteAndGo(conn_ship_log);
    }

    if (ret != ENGINE_SUCCESS) {
        c->setState(conn_closing);
    }
}

static void add_set_replace_executor(McbpConnection* c, void* packet,
                                     ENGINE_STORE_OPERATION store_op) {
    auto* req = reinterpret_cast<protocol_binary_request_set*>(packet);
    c->obtainContext<MutationCommandContext>(*c, req, store_op).drive();
}


static void add_executor(McbpConnection* c, void* packet) {
    c->setNoReply(false);
    add_set_replace_executor(c, packet, OPERATION_ADD);
}

static void addq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    add_set_replace_executor(c, packet, OPERATION_ADD);
}

static void set_executor(McbpConnection* c, void* packet) {
    c->setNoReply(false);
    add_set_replace_executor(c, packet, OPERATION_SET);
}

static void setq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    add_set_replace_executor(c, packet, OPERATION_SET);
}

static void replace_executor(McbpConnection* c, void* packet) {
    c->setNoReply(false);
    add_set_replace_executor(c, packet, OPERATION_REPLACE);
}

static void replaceq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    add_set_replace_executor(c, packet, OPERATION_REPLACE);
}

static void append_prepend_executor(McbpConnection* c,
                                    void* packet,
                                    const AppendPrependCommandContext::Mode mode) {
    auto* req = reinterpret_cast<protocol_binary_request_append*>(packet);
    c->obtainContext<AppendPrependCommandContext>(*c, req, mode).drive();
}

static void append_executor(McbpConnection* c, void* packet) {
    c->setNoReply(false);
    append_prepend_executor(c, packet,
                            AppendPrependCommandContext::Mode::Append);
}

static void appendq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    append_prepend_executor(c, packet,
                            AppendPrependCommandContext::Mode::Append);
}

static void prepend_executor(McbpConnection* c, void* packet) {
    c->setNoReply(false);
    append_prepend_executor(c, packet,
                            AppendPrependCommandContext::Mode::Prepend);
}

static void prependq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    append_prepend_executor(c, packet,
                            AppendPrependCommandContext::Mode::Prepend);
}


static void get_executor(McbpConnection* c, void* packet) {
    switch (c->getCmd()) {
    case PROTOCOL_BINARY_CMD_GETQ:
        c->setNoReply(true);
        break;
    case PROTOCOL_BINARY_CMD_GET:
        c->setNoReply(false);
        break;
    case PROTOCOL_BINARY_CMD_GETKQ:
        c->setNoReply(true);
        break;
    case PROTOCOL_BINARY_CMD_GETK:
        c->setNoReply(false);
        break;
    default:
        LOG_WARNING(c,
                    "%u: get_executor: cmd (which is %d) is not a valid GET "
                        "variant - closing connection", c->getCmd());
        c->setState(conn_closing);
        return;
    }

    process_bin_get(c, packet);
}


static void stat_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_stats*>(packet);
    c->obtainContext<StatsCommandContext>(*c, *req).drive();
}

static void isasl_refresh_executor(McbpConnection* c, void* packet) {
    c->obtainContext<SaslRefreshCommandContext>(*c).drive();
}

static void ssl_certs_refresh_executor(McbpConnection* c, void* packet) {
    // MB-22464 - We don't cache the SSL certificates in memory
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

static void verbosity_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_verbosity*>(packet);
    uint32_t level = (uint32_t)ntohl(req->message.body.level);
    if (level > MAX_VERBOSITY_LEVEL) {
        level = MAX_VERBOSITY_LEVEL;
    }
    settings.setVerbose(static_cast<int>(level));
    perform_callbacks(ON_LOG_LEVEL, NULL, NULL);
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

static void version_executor(McbpConnection* c, void*) {
    mcbp_write_response(c, get_server_version(), 0, 0,
                        (uint32_t)strlen(get_server_version()));
}

static void quit_executor(McbpConnection* c, void*) {
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
    c->setWriteAndGo(conn_closing);
}

static void quitq_executor(McbpConnection* c, void*) {
    c->setState(conn_closing);
}

static void sasl_list_mech_executor(McbpConnection* c, void*) {
    if (!c->isSaslAuthEnabled()) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED);
        return;
    }

    if (c->isSslEnabled() && settings.has.ssl_sasl_mechanisms) {
        const auto& mechs = settings.getSslSaslMechanisms();
        mcbp_write_response(c, mechs.data(), 0, 0, mechs.size());
    } else if (!c->isSslEnabled() && settings.has.sasl_mechanisms) {
        const auto& mechs = settings.getSaslMechanisms();
        mcbp_write_response(c, mechs.data(), 0, 0, mechs.size());
    } else {
        /*
         * The administrator did not configure any SASL mechanisms.
         * Go ahead and use whatever we've got in cbsasl
         */
        const char* result_string = NULL;
        unsigned int string_length = 0;

        auto ret = cbsasl_listmech(c->getSaslConn(), nullptr, nullptr, " ",
                                   nullptr, &result_string, &string_length,
                                   nullptr);

        if (ret == CBSASL_OK) {
            mcbp_write_response(c, (char*)result_string, 0, 0, string_length);
        } else {
            /* Perhaps there's a better error for this... */
            LOG_WARNING(c, "%u: Failed to list SASL mechanisms: %s", c->getId(),
                        cbsasl_strerror(c->getSaslConn(), ret));
            mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR);
            return;
        }
    }
}

static void sasl_auth_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<cb::mcbp::Request*>(packet);
    c->obtainContext<SaslAuthCommandContext>(*c, *req).drive();
}

static void noop_executor(McbpConnection* c, void*) {
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

static void flush_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<cb::mcbp::Request*>(packet);
    c->obtainContext<FlushCommandContext>(*c, *req).drive();
}

static void delete_executor(McbpConnection* c, void* packet) {
    if (c->getCmd() == PROTOCOL_BINARY_CMD_DELETEQ) {
        c->setNoReply(true);
    }

    auto* req = reinterpret_cast<protocol_binary_request_delete*>(packet);
    c->obtainContext<RemoveCommandContext>(*c, req).drive();
}

static void arithmetic_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_incr*>(packet);
    c->obtainContext<ArithmeticCommandContext>(*c, *req).drive();
}

static void arithmeticq_executor(McbpConnection* c, void* packet) {
    c->setNoReply(true);
    arithmetic_executor(c, packet);
}

static void set_ctrl_token_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_set_ctrl_token*>(packet);
    uint64_t casval = ntohll(req->message.header.request.cas);
    uint64_t newval = ntohll(req->message.body.new_cas);
    uint64_t value;

    auto ret = session_cas.cas(newval, casval, value);
    mcbp_response_handler(NULL, 0, NULL, 0, NULL, 0,
                          PROTOCOL_BINARY_RAW_BYTES,
                          engine_error_2_mcbp_protocol_error(ret),
                          value, c->getCookie());

    mcbp_write_and_free(c, &c->getDynamicBuffer());
}

static void get_ctrl_token_executor(McbpConnection* c, void*) {
    mcbp_response_handler(NULL, 0, NULL, 0, NULL, 0,
                          PROTOCOL_BINARY_RAW_BYTES,
                          PROTOCOL_BINARY_RESPONSE_SUCCESS,
                          session_cas.getCasValue(), c->getCookie());
    mcbp_write_and_free(c, &c->getDynamicBuffer());
}

static void init_complete_executor(McbpConnection* c, void* packet) {
    auto* init = reinterpret_cast<protocol_binary_request_init_complete*>(packet);
    uint64_t cas = ntohll(init->message.header.request.cas);;

    if (session_cas.increment_session_counter(cas)) {
        set_server_initialized(true);
        session_cas.decrement_session_counter();
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
        perform_callbacks(ON_INIT_COMPLETE, nullptr, nullptr);
    } else {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
    }
}

static void ioctl_get_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_ioctl_set*>(packet);
    ENGINE_ERROR_CODE ret = c->getAiostat();
    c->setAiostat(ENGINE_SUCCESS);
    c->setEwouldblock(false);

    std::string value;
    if (ret == ENGINE_SUCCESS) {
        const char* key_ptr =
                reinterpret_cast<const char*>(req->bytes + sizeof(req->bytes));
        size_t keylen = ntohs(req->message.header.request.keylen);
        const std::string key(key_ptr, keylen);

        ret = ioctl_get_property(c, key, value);
    }

    ret = c->remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        try {
            if (mcbp_response_handler(NULL, 0, NULL, 0,
                                      value.data(), value.size(),
                                      PROTOCOL_BINARY_RAW_BYTES,
                                      PROTOCOL_BINARY_RESPONSE_SUCCESS, 0,
                                      c->getCookie())) {
                mcbp_write_and_free(c, &c->getDynamicBuffer());
            } else {
                mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM);
            }
        } catch (std::exception& e) {
            LOG_WARNING(c, "ioctl_get_executor: Failed to format response: %s",
                        e.what());
            mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM);
        }
        break;
    case ENGINE_EWOULDBLOCK:
        c->setAiostat(ENGINE_EWOULDBLOCK);
        c->setEwouldblock(true);
        break;
    case ENGINE_DISCONNECT:
        c->setState(conn_closing);
        break;
    default:
        mcbp_write_packet(c, engine_error_2_mcbp_protocol_error(ret));
    }
}

static void ioctl_set_executor(McbpConnection* c, void* packet) {
    auto* req = reinterpret_cast<protocol_binary_request_ioctl_set*>(packet);

    const char* key_ptr = reinterpret_cast<const char*>(
        req->bytes + sizeof(req->bytes));
    size_t keylen = ntohs(req->message.header.request.keylen);
    const std::string key(key_ptr, keylen);

    const char* val_ptr = key_ptr + keylen;
    size_t vallen = ntohl(req->message.header.request.bodylen) - keylen;
    const std::string value(val_ptr, vallen);


    ENGINE_ERROR_CODE status = ioctl_set_property(c, key, value);

    mcbp_write_packet(c, engine_error_2_mcbp_protocol_error(status));
}

static void config_validate_executor(McbpConnection* c, void* packet) {
    const char* val_ptr = NULL;
    cJSON* errors = NULL;
    auto* req = reinterpret_cast<protocol_binary_request_ioctl_set*>(packet);

    size_t keylen = ntohs(req->message.header.request.keylen);
    size_t vallen = ntohl(req->message.header.request.bodylen) - keylen;

    /* Key not yet used, must be zero length. */
    if (keylen != 0) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL);
        return;
    }

    /* must have non-zero length config */
    if (vallen == 0 || vallen > CONFIG_VALIDATE_MAX_LENGTH) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL);
        return;
    }

    val_ptr = (const char*)(req->bytes + sizeof(req->bytes)) + keylen;

    /* null-terminate value, and convert to integer */
    try {
        std::string val_buffer(val_ptr, vallen);
        errors = cJSON_CreateArray();

        if (validate_proposed_config_changes(val_buffer.c_str(), errors)) {
            mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
        } else {
            /* problem(s). Send the errors back to the client. */
            char* error_string = cJSON_PrintUnformatted(errors);
            if (mcbp_response_handler(NULL, 0, NULL, 0, error_string,
                                      (uint32_t)strlen(error_string),
                                      PROTOCOL_BINARY_RAW_BYTES,
                                      PROTOCOL_BINARY_RESPONSE_EINVAL, 0,
                                      c->getCookie())) {
                mcbp_write_and_free(c, &c->getDynamicBuffer());
            } else {
                mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_ENOMEM);
            }
            cJSON_Free(error_string);
        }

        cJSON_Delete(errors);
    } catch (const std::bad_alloc&) {
        LOG_WARNING(c,
                    "%u: Failed to allocate buffer of size %"
                        PRIu64 " to validate config. Shutting down connection",
                    c->getId(), vallen + 1);
        c->setState(conn_closing);
        return;
    }

}

static void config_reload_executor(McbpConnection* c, void*) {
    // We need to audit that the privilege debug mode changed and
    // in order to do that we need the "connection" object so we can't
    // do this by using the common "changed_listener"-interface.
    bool old_priv_debug = settings.isPrivilegeDebug();
    reload_config_file();
    if (settings.isPrivilegeDebug() != old_priv_debug) {
        audit_set_privilege_debug_mode(c, settings.isPrivilegeDebug());
    }
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
}

static void audit_config_reload_executor(McbpConnection* c, void*) {
    c->obtainContext<AuditConfigureCommandContext>(*c).drive();
}

static void audit_put_executor(McbpConnection* c, void* packet) {

    auto* req = reinterpret_cast<const protocol_binary_request_audit_put*>(packet);
    const void* payload = req->bytes + sizeof(req->message.header) +
                          req->message.header.request.extlen;

    const size_t payload_length = ntohl(req->message.header.request.bodylen) -
                                  req->message.header.request.extlen;

    if (mc_audit_event(ntohl(req->message.body.id), payload, payload_length)) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
    } else {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINTERNAL);
    }
}

/**
 * The create bucket contains message have the following format:
 *    key: bucket name
 *    body: module\nconfig
 */
static void create_bucket_executor(McbpConnection* c, void* packet) {
    auto* req = static_cast<cb::mcbp::Request*>(packet);
    c->obtainContext<CreateRemoveBucketCommandContext>(*c, *req).drive();
}

static void delete_bucket_executor(McbpConnection* c, void* packet) {
    auto* req = static_cast<cb::mcbp::Request*>(packet);
    c->obtainContext<CreateRemoveBucketCommandContext>(*c, *req).drive();
}

static void get_errmap_executor(McbpConnection *c, void *packet) {
    auto const *req = reinterpret_cast<protocol_binary_request_get_errmap*>(packet);
    uint16_t version = ntohs(req->message.body.version);
    auto const& ss = settings.getErrorMap(version);
    if (ss.empty()) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
    } else {
        mcbp_response_handler(NULL, 0, NULL, 0, ss.data(), ss.size(),
                              PROTOCOL_BINARY_RAW_BYTES,
                              PROTOCOL_BINARY_RESPONSE_SUCCESS,
                              0, c->getCookie());
        mcbp_write_and_free(c, &c->getDynamicBuffer());
    }
}

static void shutdown_executor(McbpConnection* c, void* packet) {
    auto req = reinterpret_cast<protocol_binary_request_shutdown*>(packet);
    uint64_t cas = ntohll(req->message.header.request.cas);

    if (session_cas.increment_session_counter(cas)) {
        shutdown_server();
        session_cas.decrement_session_counter();
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_SUCCESS);
    } else {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
    }
}

static void rbac_refresh_executor(McbpConnection* c, void*) {
    c->obtainContext<RbacReloadCommandContext>(*c).drive();
}

static void no_support_executor(McbpConnection* c, void*) {
    mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED);
}

std::array<mcbp_package_execute, 0x100>& get_mcbp_executors(void) {
    static std::array<mcbp_package_execute, 0x100> executors;
    std::fill(executors.begin(), executors.end(), nullptr);

    executors[PROTOCOL_BINARY_CMD_DCP_OPEN] = dcp_open_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_ADD_STREAM] = dcp_add_stream_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM] = dcp_close_stream_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER] = dcp_snapshot_marker_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_DELETION] = dcp_deletion_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_EXPIRATION] = dcp_expiration_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_FLUSH] = dcp_flush_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG] = dcp_get_failover_log_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_MUTATION] = dcp_mutation_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE] = dcp_set_vbucket_state_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_NOOP] = dcp_noop_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT] = dcp_buffer_acknowledgement_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_CONTROL] = dcp_control_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_STREAM_END] = dcp_stream_end_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_STREAM_REQ] = dcp_stream_req_executor;
    executors[PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT] = dcp_system_event_executor;
    executors[PROTOCOL_BINARY_CMD_ISASL_REFRESH] = isasl_refresh_executor;
    executors[PROTOCOL_BINARY_CMD_SSL_CERTS_REFRESH] = ssl_certs_refresh_executor;
    executors[PROTOCOL_BINARY_CMD_VERBOSITY] = verbosity_executor;
    executors[PROTOCOL_BINARY_CMD_HELLO] = process_hello_packet_executor;
    executors[PROTOCOL_BINARY_CMD_VERSION] = version_executor;
    executors[PROTOCOL_BINARY_CMD_QUIT] = quit_executor;
    executors[PROTOCOL_BINARY_CMD_QUITQ] = quitq_executor;
    executors[PROTOCOL_BINARY_CMD_SASL_LIST_MECHS] = sasl_list_mech_executor;
    executors[PROTOCOL_BINARY_CMD_SASL_AUTH] = sasl_auth_executor;
    executors[PROTOCOL_BINARY_CMD_SASL_STEP] = sasl_auth_executor;
    executors[PROTOCOL_BINARY_CMD_NOOP] = noop_executor;
    executors[PROTOCOL_BINARY_CMD_FLUSH] = flush_executor;
    executors[PROTOCOL_BINARY_CMD_FLUSHQ] = flush_executor;
    executors[PROTOCOL_BINARY_CMD_SETQ] = setq_executor;
    executors[PROTOCOL_BINARY_CMD_SET] = set_executor;
    executors[PROTOCOL_BINARY_CMD_ADDQ] = addq_executor;
    executors[PROTOCOL_BINARY_CMD_ADD] = add_executor;
    executors[PROTOCOL_BINARY_CMD_REPLACEQ] = replaceq_executor;
    executors[PROTOCOL_BINARY_CMD_REPLACE] = replace_executor;
    executors[PROTOCOL_BINARY_CMD_APPENDQ] = appendq_executor;
    executors[PROTOCOL_BINARY_CMD_APPEND] = append_executor;
    executors[PROTOCOL_BINARY_CMD_PREPENDQ] = prependq_executor;
    executors[PROTOCOL_BINARY_CMD_PREPEND] = prepend_executor;
    executors[PROTOCOL_BINARY_CMD_GET] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETQ] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETK] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GETKQ] = get_executor;
    executors[PROTOCOL_BINARY_CMD_GAT] = gat_executor;
    executors[PROTOCOL_BINARY_CMD_GATQ] = gat_executor;
    executors[PROTOCOL_BINARY_CMD_TOUCH] = gat_executor;
    executors[PROTOCOL_BINARY_CMD_DELETE] = delete_executor;
    executors[PROTOCOL_BINARY_CMD_DELETEQ] = delete_executor;
    executors[PROTOCOL_BINARY_CMD_STAT] = stat_executor;
    executors[PROTOCOL_BINARY_CMD_INCREMENT] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_INCREMENTQ] = arithmeticq_executor;
    executors[PROTOCOL_BINARY_CMD_DECREMENT] = arithmetic_executor;
    executors[PROTOCOL_BINARY_CMD_DECREMENTQ] = arithmeticq_executor;
    executors[PROTOCOL_BINARY_CMD_GET_CMD_TIMER] = get_cmd_timer_executor;
    executors[PROTOCOL_BINARY_CMD_SET_CTRL_TOKEN] = set_ctrl_token_executor;
    executors[PROTOCOL_BINARY_CMD_GET_CTRL_TOKEN] = get_ctrl_token_executor;
    executors[PROTOCOL_BINARY_CMD_INIT_COMPLETE] = init_complete_executor;
    executors[PROTOCOL_BINARY_CMD_IOCTL_GET] = ioctl_get_executor;
    executors[PROTOCOL_BINARY_CMD_IOCTL_SET] = ioctl_set_executor;
    executors[PROTOCOL_BINARY_CMD_CONFIG_VALIDATE] = config_validate_executor;
    executors[PROTOCOL_BINARY_CMD_CONFIG_RELOAD] = config_reload_executor;
    executors[PROTOCOL_BINARY_CMD_AUDIT_PUT] = audit_put_executor;
    executors[PROTOCOL_BINARY_CMD_AUDIT_CONFIG_RELOAD] = audit_config_reload_executor;
    executors[PROTOCOL_BINARY_CMD_SHUTDOWN] = shutdown_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_GET] = subdoc_get_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_EXISTS] = subdoc_exists_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_DICT_ADD] = subdoc_dict_add_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_DICT_UPSERT] = subdoc_dict_upsert_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_DELETE] = subdoc_delete_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_REPLACE] = subdoc_replace_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_LAST] = subdoc_array_push_last_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_PUSH_FIRST] = subdoc_array_push_first_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_INSERT] = subdoc_array_insert_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_ARRAY_ADD_UNIQUE] = subdoc_array_add_unique_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_COUNTER] = subdoc_counter_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_MULTI_LOOKUP] = subdoc_multi_lookup_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_MULTI_MUTATION] = subdoc_multi_mutation_executor;
    executors[PROTOCOL_BINARY_CMD_SUBDOC_GET_COUNT] = subdoc_get_count_executor;

    executors[PROTOCOL_BINARY_CMD_CREATE_BUCKET] = create_bucket_executor;
    executors[PROTOCOL_BINARY_CMD_LIST_BUCKETS] = list_bucket_executor;
    executors[PROTOCOL_BINARY_CMD_DELETE_BUCKET] = delete_bucket_executor;
    executors[PROTOCOL_BINARY_CMD_SELECT_BUCKET] = select_bucket_executor;
    executors[PROTOCOL_BINARY_CMD_GET_ERROR_MAP] = get_errmap_executor;
    executors[PROTOCOL_BINARY_CMD_GET_LOCKED] = get_locked_executor;
    executors[PROTOCOL_BINARY_CMD_UNLOCK_KEY] = unlock_executor;

    executors[PROTOCOL_BINARY_CMD_DROP_PRIVILEGE] = drop_privilege_executor;
    executors[PROTOCOL_BINARY_CMD_RBAC_REFRESH] = rbac_refresh_executor;
    executors[PROTOCOL_BINARY_CMD_COLLECTIONS_SET_MANIFEST] =
            collections_set_manifest_executor;

    executors[PROTOCOL_BINARY_CMD_TAP_CONNECT] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_MUTATION] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_DELETE] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_FLUSH] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_OPAQUE] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_VBUCKET_SET] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_START] = no_support_executor;
    executors[PROTOCOL_BINARY_CMD_TAP_CHECKPOINT_END] = no_support_executor;

    return executors;
}

static void process_bin_dcp_response(McbpConnection* c) {
    ENGINE_ERROR_CODE ret = ENGINE_DISCONNECT;

    c->enableDatatype(mcbp::Feature::SNAPPY);
    c->enableDatatype(mcbp::Feature::JSON);

    if (c->getBucketEngine()->dcp.response_handler != NULL) {
        auto* header = reinterpret_cast<protocol_binary_response_header*>(
                c->getPacket(c->getCookieObject()));
        ret = c->getBucketEngine()->dcp.response_handler
            (c->getBucketEngineAsV0(), c->getCookie(), header);
        ret = c->remapErrorCode(ret);
    }

    if (ret == ENGINE_DISCONNECT) {
        c->setState(conn_closing);
    } else {
        c->setState(conn_ship_log);
    }
}


void initialize_mbcp_lookup_map(void) {
    int ii;
    for (ii = 0; ii < 0x100; ++ii) {
        request_handlers[ii].descriptor = NULL;
        request_handlers[ii].callback = default_unknown_command;
    }

    response_handlers[PROTOCOL_BINARY_CMD_NOOP] = process_bin_noop_response;

    response_handlers[PROTOCOL_BINARY_CMD_DCP_OPEN] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_ADD_STREAM] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_CLOSE_STREAM] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_STREAM_REQ] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_GET_FAILOVER_LOG] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_STREAM_END] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_MUTATION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_DELETION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_EXPIRATION] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_FLUSH] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_NOOP] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_CONTROL] = process_bin_dcp_response;
    response_handlers[PROTOCOL_BINARY_CMD_DCP_SYSTEM_EVENT] =
            process_bin_dcp_response;
}

/**
 * Check if the current packet use an invalid datatype value. It may be
 * considered invalid for two reasons:
 *
 *    1) it is using an unknown value
 *    2) The connected client has not enabled the datatype
 *    3) The bucket has disabled the datatype
 *
 * @param c - the connected client
 * @return true if the packet is considered invalid in this context,
 *         false otherwise
 */
static bool invalid_datatype(McbpConnection* c) {
    return !c->isDatatypeEnabled(c->binary_header.request.datatype);
}

static protocol_binary_response_status validate_bin_header(McbpConnection* c) {
    if (c->binary_header.request.bodylen >=
        (c->binary_header.request.keylen + c->binary_header.request.extlen)) {
        return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    } else {
        return PROTOCOL_BINARY_RESPONSE_EINVAL;
    }
}

static void execute_request_packet(McbpConnection* c) {
    static McbpPrivilegeChains privilegeChains;
    protocol_binary_response_status result;

    char* packet = static_cast<char*>(c->getPacket(c->getCookieObject()));
    auto opcode = static_cast<protocol_binary_command>(c->binary_header.request.opcode);
    auto executor = executors[opcode];

    const auto res = privilegeChains.invoke(opcode, c->getCookieObject());
    switch (res) {
    case cb::rbac::PrivilegeAccess::Fail:
        LOG_WARNING(c,
                    "%u %s: no access to command %s",
                    c->getId(), c->getDescription().c_str(),
                    memcached_opcode_2_text(opcode));
        audit_command_access_failed(c);

        if (c->remapErrorCode(ENGINE_EACCESS) == ENGINE_DISCONNECT) {
            c->setState(conn_closing);
            return;
        } else {
            mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EACCESS);
        }

        return;
    case cb::rbac::PrivilegeAccess::Ok:
        result = validate_bin_header(c);
        if (result == PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            result = c->validateCommand(opcode);
        }

        if (result != PROTOCOL_BINARY_RESPONSE_SUCCESS) {
            LOG_NOTICE(c,
                       "%u: Invalid format specified for %s - %d - "
                           "closing connection",
                       c->getId(), memcached_opcode_2_text(opcode), result);
            audit_invalid_packet(c);
            mcbp_write_packet(c, result);
            c->setWriteAndGo(conn_closing);
            return;
        }

        if (executor != NULL) {
            executor(c, packet);
        } else {
            process_bin_unknown_packet(c, packet);
        }
        return;
    case cb::rbac::PrivilegeAccess::Stale:
        if (c->remapErrorCode(ENGINE_AUTH_STALE) == ENGINE_DISCONNECT) {
            c->setState(conn_closing);
        } else {
            mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_STALE);
        }
        return;
    }

    LOG_WARNING(c,
                "%u: execute_request_packet: res (which is %d) is not a valid "
                "AuthResult - closing connection",
                res);
    c->setState(conn_closing);
}

/**
 * We've received a response packet. Parse and execute it
 *
 * @param c The connection receiving the packet
 */
static void execute_response_packet(McbpConnection* c) {
    auto handler = response_handlers[c->binary_header.request.opcode];
    if (handler) {
        handler(c);
    } else {
        LOG_NOTICE(c,
                   "%u: Unsupported response packet received with opcode: %02x",
                   c->getId(),
                   c->binary_header.request.opcode);
        c->setState(conn_closing);
    }
}

static inline bool is_initialized(McbpConnection* c, uint8_t opcode) {
    if (c->isInternal() || is_server_initialized()) {
        return true;
    }

    switch (opcode) {
    case PROTOCOL_BINARY_CMD_SASL_LIST_MECHS:
    case PROTOCOL_BINARY_CMD_SASL_AUTH:
    case PROTOCOL_BINARY_CMD_SASL_STEP:
        return true;
    default:
        return false;
    }
}

static void dispatch_bin_command(McbpConnection* c) {
    uint16_t keylen = c->binary_header.request.keylen;

    /* @trond this should be in the Connection-connect part.. */
    /*        and in the select bucket */
    if (c->getBucketEngine() == NULL) {
        c->setBucketEngine(all_buckets[c->getBucketIndex()].engine);
    }

    if (!is_initialized(c, c->binary_header.request.opcode)) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_NOT_INITIALIZED);
        c->setWriteAndGo(conn_closing);
        return;
    }

    if (settings.isRequireSasl() && !authenticated(c)) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_AUTH_ERROR);
        c->setWriteAndGo(conn_closing);
        return;
    }

    if (invalid_datatype(c)) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL);
        c->setWriteAndGo(conn_closing);
        return;
    }

    if (c->getStart() == 0) {
        c->setStart(gethrtime());
    }

    MEMCACHED_PROCESS_COMMAND_START(c->getId(), c->read.curr, c->read.bytes);

    /* binprot supports 16bit keys, but internals are still 8bit */
    if (keylen > KEY_MAX_LENGTH) {
        handle_binary_protocol_error(c);
        return;
    }

    c->setNoReply(false);

    /*
     * Protect ourself from someone trying to kill us by sending insanely
     * large packets.
     */
    if (c->binary_header.request.bodylen > settings.getMaxPacketSize()) {
        mcbp_write_packet(c, PROTOCOL_BINARY_RESPONSE_EINVAL);
        c->setWriteAndGo(conn_closing);
    } else {
        bin_read_chunk(c, c->binary_header.request.bodylen);
    }
}

void mcbp_complete_packet(McbpConnection* c) {
    if (c->binary_header.request.magic == PROTOCOL_BINARY_RES) {
        execute_response_packet(c);
    } else {
        // We've already verified that the packet is a legal packet
        // so it must be a request
        execute_request_packet(c);
    }
}

void try_read_mcbp_command(McbpConnection* c) {
    if (c == nullptr) {
        throw std::runtime_error("Internal eror, connection is not mcbp");
    }
    cb_assert(c->read.curr <= (c->read.buf + c->read.size));
    cb_assert(c->read.bytes >= sizeof(c->binary_header));

    /* Do we have the complete packet header? */
#ifdef NEED_ALIGN
    if (((long)(c->read.curr)) % 8 != 0) {
        /* must realign input buffer */
        memmove(c->read.buf, c->read.curr, c->read.bytes);
        c->read.curr = c->read.buf;
        LOG_DEBUG(c, "%d: Realign input buffer", c->sfd);
    }
#endif
    protocol_binary_request_header* req;
    req = (protocol_binary_request_header*)c->read.curr;

    if (settings.getVerbose() > 1) {
        /* Dump the packet before we convert it to host order */
        char buffer[1024];
        ssize_t nw;
        nw = bytes_to_output_string(buffer, sizeof(buffer), c->getId(),
                                    true, "Read binary protocol data:",
                                    (const char*)req->bytes,
                                    sizeof(req->bytes));
        if (nw != -1) {
            LOG_DEBUG(c, "%s", buffer);
        }
    }

    c->binary_header = *req;
    c->binary_header.request.keylen = ntohs(req->request.keylen);
    c->binary_header.request.bodylen = ntohl(req->request.bodylen);
    c->binary_header.request.vbucket = ntohs(req->request.vbucket);
    c->binary_header.request.cas = ntohll(req->request.cas);

    if (c->binary_header.request.magic != PROTOCOL_BINARY_REQ &&
        !(c->binary_header.request.magic == PROTOCOL_BINARY_RES &&
          response_handlers[c->binary_header.request.opcode])) {
        if (c->binary_header.request.magic != PROTOCOL_BINARY_RES) {
            LOG_WARNING(c, "%u: Invalid magic: %x, closing connection",
                        c->getId(), c->binary_header.request.magic);
        } else {
            LOG_WARNING(c,
                        "%u: Unsupported response packet received: %u, "
                            "closing connection",
                        c->getId(),
                        (unsigned int)c->binary_header.request.opcode);

        }
        c->setState(conn_closing);
        return;
    }

    c->addMsgHdr(true);
    c->setCmd(c->binary_header.request.opcode);
    /* clear the returned cas value */
    c->setCAS(0);

    dispatch_bin_command(c);

    c->read.bytes -= sizeof(c->binary_header);
    c->read.curr += sizeof(c->binary_header);
}
