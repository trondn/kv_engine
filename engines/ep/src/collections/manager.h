/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
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

#include <memcached/engine_error.h>

#include <memory>
#include <mutex>

class KVBucket;
class VBucket;

namespace Collections {

class Manifest;
class Filter;

/**
 * Collections::Manager provides some bucket level management functions
 * such as the code which enables the MCBP set_collections command.
 */
class Manager {
public:
    Manager();

    /**
     * Update the bucket with the latest JSON collections manifest.
     *
     * Locks the Manager and prevents concurrent updates, concurrent updates
     * are failed with TMPFAIL as in reality there should be 1 admin connection.
     *
     * @param bucket the bucket receiving a set-collections command.
     * @param json the json manifest form a set-collections command.
     * @returns engine_error indicating why the update failed.
     */
    cb::engine_error update(KVBucket& bucket, const std::string& json);

    /**
     * Update the vbucket's manifest with the current Manifest
     * The Manager is locked to prevent current changing whilst this update
     * occurs.
     */
    void update(VBucket& vb) const;

    /**
     * make a Collections::Filter
     * @throws for bad json or incorrect collections
     */
    std::unique_ptr<Filter> makeFilter(bool collectionsEnabled,
                                       const std::string& json) const;

    /**
     * For development, log as much collections stuff as we can
     */
    void logAll(KVBucket& bucket) const;

    /**
     * Write to std::cerr this
     */
    void dump() const;

private:
    friend std::ostream& operator<<(std::ostream& os, const Manager& manager);

    mutable std::mutex lock;

    /// Store the most recent (current) manifest received
    std::unique_ptr<Manifest> current;
};

std::ostream& operator<<(std::ostream& os, const Manager& manager);
}