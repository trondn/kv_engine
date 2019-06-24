/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2019 Couchbase, Inc.
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

#include "engine_errc_2_mcbp.h"
#include "executors.h"

#include <daemon/cookie.h>
#include <mcbp/protocol/request.h>
#include <memcached/engine.h>

void collections_get_scope_id_executor(Cookie& cookie) {
    auto& connection = cookie.getConnection();
    auto& req = cookie.getRequest();
    auto key = req.getKey();
    cb::const_char_buffer path{reinterpret_cast<const char*>(key.data()),
                               key.size()};
    auto rv = connection.getBucketEngine()->get_scope_id(&cookie, path);

    auto remapErr = connection.remapErrorCode(rv.result);

    if (remapErr == cb::engine_errc::disconnect) {
        connection.setState(StateMachine::State::closing);
        return;
    }

    if (remapErr == cb::engine_errc::success) {
        cookie.sendResponse(cb::mcbp::Status::Success,
                            {rv.extras.bytes, sizeof(rv.extras.bytes)},
                            {},
                            {},
                            cb::mcbp::Datatype::Raw,
                            0);
    } else {
        cookie.sendResponse(remapErr);
    }
}
