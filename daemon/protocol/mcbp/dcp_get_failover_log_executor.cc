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

#include "dcp_add_failover_log.h"
#include "engine_wrapper.h"

#include <daemon/cookie.h>
#include <mcbp/protocol/header.h>
#include <mcbp/protocol/request.h>

void dcp_get_failover_log_executor(Cookie& cookie) {
    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        auto& header = cookie.getHeader().getRequest();
        ret = dcpGetFailoverLog(cookie,
                                header.getOpaque(),
                                header.getVBucket(),
                                add_failover_log);
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        if (cookie.getDynamicBuffer().getRoot() != nullptr) {
            cookie.sendDynamicBuffer();
        } else {
            cookie.sendResponse(cb::mcbp::Status::Success);
        }
        break;

    case ENGINE_DISCONNECT:
        connection.shutdown();
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
