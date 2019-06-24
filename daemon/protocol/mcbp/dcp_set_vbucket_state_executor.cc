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

#include "executors.h"

#include "engine_wrapper.h"

#include <daemon/cookie.h>
#include <memcached/protocol_binary.h>

void dcp_set_vbucket_state_executor(Cookie& cookie) {
    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        using cb::mcbp::request::DcpSetVBucketState;
        auto& request = cookie.getRequest();
        auto extras = request.getExtdata();
        const auto* payload =
                reinterpret_cast<const DcpSetVBucketState*>(extras.data());
        ret = dcpSetVbucketState(cookie,
                                 request.getOpaque(),
                                 request.getVBucket(),
                                 vbucket_state_t(payload->getState()));
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        break;
    case ENGINE_DISCONNECT:
        connection.shutdown();
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        connection.shutdown();
        break;
    }
}
