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

#pragma once

/*
 * Memcached binary protocol validator tests.
 */

#include "config.h"

#include <daemon/connection_mcbp.h>
#include <daemon/mcbp_validators.h>
#include <daemon/stats.h>
#include <gtest/gtest.h>

namespace mcbp {
namespace test {

class ValidatorTest : public ::testing::Test {
public:
    ValidatorTest();
    void SetUp() override;

protected:
    /**
     * Validate that the provided packet is correctly encoded
     *
     * @param opcode The opcode for the packet
     * @param request The packet to validate
     */
    protocol_binary_response_status validate(protocol_binary_command opcode,
                                             void* request);

    McbpValidatorChains validatorChains;

    /**
     * Create a mock connection which doesn't own a socket and isn't bound
     * to libevent
     */
    class MockConnection : public McbpConnection {
    public:
        MockConnection() : McbpConnection() {
        }
    };
    MockConnection connection;

    // backing store which may be used for the request
    protocol_binary_request_no_extras &request;
    uint8_t blob[4096];
};

} // namespace test
} // namespace mcbp
