/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
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

#include "datatype.h"

#include <boost/optional.hpp>
#include <mcbp/protocol/header.h>
#include <mcbp/protocol/magic.h>
#include <mcbp/protocol/opcode.h>
#include <memcached/vbucket.h>
#include <platform/sized_buffer.h>

#ifndef WIN32
#include <arpa/inet.h>
#endif

#include <cstdint>
#include <functional>

namespace cb {
namespace durability {
class Requirements;
}
} // namespace cb

namespace cb {
namespace mcbp {

namespace request {
enum class FrameInfoId {
    Reorder = 0,
    DurabilityRequirement = 1,
    DcpStreamId = 2,
    OpenTracingContext = 3,
};
}

/**
 * Definition of the header structure for a request packet.
 * See section 2
 */
class Request {
public:
    void setMagic(Magic magic) {
        if (is_request(magic)) {
            Request::magic = uint8_t(magic);
        } else {
            throw std::invalid_argument(
                    "Request::setMagic: Invalid magic specified: " +
                    std::to_string(uint8_t(magic)));
        }
    }

    Magic getMagic() const {
        return Magic(magic);
    }

    void setOpcode(ClientOpcode opcode) {
        Request::opcode = uint8_t(opcode);
    }

    ClientOpcode getClientOpcode() const {
        if (is_server_magic(getMagic())) {
            throw std::logic_error("getClientOpcode: magic != client request");
        }
        return ClientOpcode(opcode);
    }

    void setOpcode(ServerOpcode opcode) {
        Request::opcode = uint8_t(opcode);
    }

    ServerOpcode getServerOpcode() const {
        if (is_client_magic(getMagic())) {
            throw std::logic_error(
                    "getServerOpcode: magic != server request: " +
                    std::to_string(uint8_t(getMagic())));
        }
        return ServerOpcode(opcode);
    }

    void setKeylen(uint16_t value);

    uint16_t getKeylen() const {
        return reinterpret_cast<const Header*>(this)->getKeylen();
    }

    uint8_t getFramingExtraslen() const {
        return reinterpret_cast<const Header*>(this)->getFramingExtraslen();
    }

    void setFramingExtraslen(uint8_t len);

    void setExtlen(uint8_t extlen) {
        Request::extlen = extlen;
    }

    uint8_t getExtlen() const {
        return reinterpret_cast<const Header*>(this)->getExtlen();
    }

    void setDatatype(Datatype datatype) {
        Request::datatype = uint8_t(datatype);
    }

    Datatype getDatatype() const {
        return Datatype(datatype);
    }

    void setVBucket(Vbid value) {
        vbucket = value.hton();
    }

    Vbid getVBucket() const {
        return vbucket.ntoh();
    }

    uint32_t getBodylen() const {
        return reinterpret_cast<const Header*>(this)->getBodylen();
    }

    void setBodylen(uint32_t value) {
        bodylen = htonl(value);
    }

    void setOpaque(uint32_t opaque) {
        Request::opaque = opaque;
    }

    uint32_t getOpaque() const {
        return reinterpret_cast<const Header*>(this)->getOpaque();
    }

    uint64_t getCas() const {
        return reinterpret_cast<const Header*>(this)->getCas();
    }

    void setCas(uint64_t val) {
        cas = htonll(val);
    }

    /**
     * Get a printable version of the key (non-printable characters replaced
     * with a '.'
     */
    std::string getPrintableKey() const;

    cb::const_byte_buffer getFramingExtras() const {
        return reinterpret_cast<const Header*>(this)->getFramingExtras();
    }

    cb::const_byte_buffer getExtdata() const {
        return reinterpret_cast<const Header*>(this)->getExtdata();
    }

    cb::const_byte_buffer getKey() const {
        return reinterpret_cast<const Header*>(this)->getKey();
    }

    cb::const_byte_buffer getValue() const {
        return reinterpret_cast<const Header*>(this)->getValue();
    }

    cb::const_byte_buffer getFrame() const {
        return reinterpret_cast<const Header*>(this)->getFrame();
    }

    /**
     * Callback function to use while parsing the FrameExtras section
     *
     * The first parameter is the identifier for the frame info, the
     * second parameter is the content of the frame info.
     *
     * If the callback function should return false if it wants to stop
     * further parsing of the FrameExtras
     */
    using FrameInfoCallback = std::function<bool(cb::mcbp::request::FrameInfoId,
                                                 cb::const_byte_buffer)>;
    /**
     * Iterate over the provided frame extras
     *
     * @param callback The callback function to call for each frame info found.
     *                 if the callback returns false we stop parsing the
     *                 frame extras. Provided to the callback is the
     *                 Frame Info identifier and the content
     * @throws std::overflow_error if we overflow the encoded buffer.
     */
    void parseFrameExtras(FrameInfoCallback callback) const;

    /**
     * Parse the Frame Extras section and pick out the optional Durability
     * spec associated with the command.
     *
     * This method may throw exceptions iff the request object has not been
     * inspected by the packet validators.
     */
    boost::optional<cb::durability::Requirements> getDurabilityRequirements()
            const;

    /**
     * Is this a quiet command or not
     */
    bool isQuiet() const;

    nlohmann::json toJSON() const;

    /**
     * Validate that the header is "sane" (correct magic, and extlen+keylen
     * doesn't exceed the body size)
     */
    bool isValid() const;

protected:
    uint8_t magic;
    uint8_t opcode;
    uint16_t keylen;
    uint8_t extlen;
    uint8_t datatype;
    Vbid vbucket;
    uint32_t bodylen;
    uint32_t opaque;

public:
    // We still have some unit tests which (from the looks of it) seems
    // to access this in network byte order and move it around
    uint64_t cas;
};

static_assert(sizeof(Request) == 24, "Incorrect compiler padding");

} // namespace mcbp
} // namespace cb

std::string to_string(cb::mcbp::request::FrameInfoId id);
