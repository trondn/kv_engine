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
#include "dcp_system_event_executor.h"
#include "engine_wrapper.h"
#include "utilities.h"

#include <memcached/protocol_binary.h>

void dcp_system_event_executor(Cookie& cookie) {
    auto ret = cookie.swapAiostat(ENGINE_SUCCESS);

    auto& connection = cookie.getConnection();
    if (ret == ENGINE_SUCCESS) {
        using cb::mcbp::request::DcpSystemEventPayload;
        const auto& request = cookie.getRequest();
        auto extras = request.getExtdata();
        const auto* payload =
                reinterpret_cast<const DcpSystemEventPayload*>(extras.data());

        ret = dcpSystemEvent(cookie,
                             request.getOpaque(),
                             request.getVBucket(),
                             mcbp::systemevent::id(payload->getEvent()),
                             payload->getBySeqno(),
                             mcbp::systemevent::version(payload->getVersion()),
                             request.getKey(),
                             request.getValue());
    }

    ret = connection.remapErrorCode(ret);
    switch (ret) {
    case ENGINE_SUCCESS:
        connection.setState(StateMachine::State::new_cmd);
        break;

    case ENGINE_DISCONNECT:
        connection.setState(StateMachine::State::closing);
        break;

    case ENGINE_EWOULDBLOCK:
        cookie.setEwouldblock(true);
        break;

    default:
        cookie.sendResponse(cb::engine_errc(ret));
    }
}
