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

#include <memcached/engine.h>
#include <platform/compression/buffer.h>
#include <platform/sized_buffer.h>

class Bucket;

/**
 * Abstract class for a generic send buffer to be passed to libevent
 * which holds some data allocated elsewhere which needs to be released
 * when libevent is done sending the data
 */
class SendBuffer {
public:
    explicit SendBuffer(cb::const_char_buffer view) : payload(view) {
    }
    virtual ~SendBuffer() = default;
    virtual cb::const_char_buffer getPayload() {
        return payload;
    }

protected:
    const cb::const_char_buffer payload;
};

/**
 * Specialized send buffer which holds an item which needs to be
 * released once libevent is done sending any data held by the
 * object.
 */
class ItemSendBuffer : public SendBuffer {
public:
    /**
     * @param itm The item send (the ownership of the item is
     *            transferred to this object)
     * @param view The memory area within the item to transfer
     * @param bucket The bucket the item belongs to
     */
    ItemSendBuffer(cb::unique_item_ptr& itm,
                   cb::const_char_buffer view,
                   Bucket& bucket);

    ~ItemSendBuffer() override;

protected:
    cb::unique_item_ptr item;
    Bucket& bucket;
};

/**
 * Specialized class to send a buffer allocated by the compression
 * framework.
 */
class CompressionSendBuffer : public SendBuffer {
public:
    CompressionSendBuffer(cb::compression::Buffer& buffer,
                          cb::const_char_buffer view)
        : SendBuffer(view),
          allocator(buffer.allocator),
          data(buffer.release()) {
    }

    ~CompressionSendBuffer() override {
        allocator.deallocate(data);
    }

protected:
    cb::compression::Allocator allocator;
    char* data;
};

/**
 * Specialized class to send a character buffer
 */
class CharBufferSendBuffer : public SendBuffer {
public:
    CharBufferSendBuffer(std::unique_ptr<char[]>& blob,
                         cb::const_char_buffer view)
        : SendBuffer(view), data(std::move(blob)) {
    }

    ~CharBufferSendBuffer() override = default;

protected:
    std::unique_ptr<char[]> data;
};
