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

#include "durability_monitor_test.h"
#include "../mock/mock_synchronous_ep_engine.h"

ENGINE_ERROR_CODE DurabilityMonitorTest::addSyncWrite(int64_t seqno) {
    auto item = std::make_unique<Item>(
            makeStoredDocKey("key" + std::to_string(seqno)),
            0 /*flags*/,
            0 /*exp*/,
            "value",
            5 /*valueSize*/,
            PROTOCOL_BINARY_RAW_BYTES,
            0 /*cas*/,
            seqno);
    queued_item qi(std::move(item));
    using namespace cb::durability;
    qi->setPendingSyncWrite(Requirements(Level::Majority, 0 /*timeout*/));
    return monitor->addSyncWrite(qi);
}

size_t DurabilityMonitorTest::addSyncWrites(int64_t seqnoStart,
                                            int64_t seqnoEnd) {
    size_t expectedNumTracked = monitor->public_getNumTracked();
    size_t added = 0;
    for (auto seqno = seqnoStart; seqno <= seqnoEnd; seqno++) {
        EXPECT_EQ(ENGINE_SUCCESS, addSyncWrite(seqno));
        added++;
        expectedNumTracked++;
        EXPECT_EQ(expectedNumTracked, monitor->public_getNumTracked());
    }
    return added;
}

size_t DurabilityMonitorTest::addSyncWrites(
        const std::vector<int64_t>& seqnos) {
    size_t expectedNumTracked = monitor->public_getNumTracked();
    size_t added = 0;
    for (auto seqno : seqnos) {
        EXPECT_EQ(ENGINE_SUCCESS, addSyncWrite(seqno));
        added++;
        expectedNumTracked++;
        EXPECT_EQ(expectedNumTracked, monitor->public_getNumTracked());
    }
    return added;
}

TEST_F(DurabilityMonitorTest, AddSyncWrite) {
    EXPECT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedNoTrackedSyncWrite) {
    try {
        monitor->seqnoAckReceived(replica, 1 /*memSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedSmallerThanPending) {
    EXPECT_EQ(ENGINE_SUCCESS, addSyncWrite(1 /*seqno*/));
    try {
        auto seqno = monitor->public_getReplicaMemorySyncWriteSeqno(replica);
        monitor->seqnoAckReceived(replica, seqno - 1 /*memSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find(
                            "Ack'ed seqno is behind pending seqno") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedEqualPending) {
    int64_t seqnoStart = 1;
    int64_t seqnoEnd = 3;
    auto numItems = addSyncWrites(seqnoStart, seqnoEnd);
    ASSERT_EQ(3, numItems);
    ASSERT_EQ(0, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    ASSERT_EQ(0, monitor->public_getReplicaMemoryAckSeqno(replica));

    for (int64_t seqno = seqnoStart; seqno <= seqnoEnd; seqno++) {
        EXPECT_NO_THROW(monitor->seqnoAckReceived(replica, seqno /*memSeqno*/));
        // Check that the tracking advances by 1 at each cycle
        EXPECT_EQ(seqno,
                  monitor->public_getReplicaMemorySyncWriteSeqno(replica));
        EXPECT_EQ(seqno, monitor->public_getReplicaMemoryAckSeqno(replica));
        // Check that we committed and removed 1 SyncWrite
        EXPECT_EQ(--numItems, monitor->public_getNumTracked());
        // Check that seqno-tracking is not lost after commit+remove
        EXPECT_EQ(seqno,
                  monitor->public_getReplicaMemorySyncWriteSeqno(replica));
        EXPECT_EQ(seqno, monitor->public_getReplicaMemoryAckSeqno(replica));
    }

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, seqnoEnd + 1 /*memSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanPending_ContinuousSeqnos) {
    ASSERT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
    ASSERT_EQ(0, monitor->public_getReplicaMemorySyncWriteSeqno(replica));

    int64_t memoryAckSeqno = 2;
    // Receive a seqno-ack in the middle of tracked seqnos
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(replica, memoryAckSeqno));
    // Check that the tracking has advanced to the ack'ed seqno
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
    // Check that we committed and removed 2 SyncWrites
    EXPECT_EQ(1, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
}

TEST_F(DurabilityMonitorTest, SeqnoAckReceivedGreaterThanPending_SparseSeqnos) {
    ASSERT_EQ(3, addSyncWrites({1, 3, 5} /*seqnos*/));
    ASSERT_EQ(0, monitor->public_getReplicaMemorySyncWriteSeqno(replica));

    int64_t memoryAckSeqno = 4;
    // Receive a seqno-ack in the middle of tracked seqnos
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(replica, memoryAckSeqno));
    // Check that the tracking has advanced to the last tracked seqno before
    // the ack'ed seqno
    EXPECT_EQ(3, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
    // Check that we committed and removed 2 SyncWrites
    EXPECT_EQ(1, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(3, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_ContinuousSeqnos) {
    ASSERT_EQ(3, addSyncWrites(1 /*seqnoStart*/, 3 /*seqnoEnd*/));
    ASSERT_EQ(0, monitor->public_getReplicaMemorySyncWriteSeqno(replica));

    int64_t memoryAckSeqno = 4;
    // Receive a seqno-ack greater than the last tracked seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(replica, memoryAckSeqno));
    // Check that the tracking has advanced to the last tracked seqno
    EXPECT_EQ(3, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
    // Check that we committed and removed all SyncWrites
    EXPECT_EQ(0, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(3, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, 20 /*memSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}

TEST_F(DurabilityMonitorTest,
       SeqnoAckReceivedGreaterThanLastTracked_SparseSeqnos) {
    ASSERT_EQ(3, addSyncWrites({1, 3, 5} /*seqnos*/));
    ASSERT_EQ(0, monitor->public_getReplicaMemorySyncWriteSeqno(replica));

    int64_t memoryAckSeqno = 10;
    // Receive a seqno-ack greater than the last tracked seqno
    EXPECT_EQ(ENGINE_SUCCESS,
              monitor->seqnoAckReceived(replica, memoryAckSeqno));
    // Check that the tracking has advanced to the last tracked seqno
    EXPECT_EQ(5, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    // Check that the ack-seqno has been updated correctly
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));
    // Check that we committed and removed all SyncWrites
    EXPECT_EQ(0, monitor->public_getNumTracked());
    // Check that seqno-tracking is not lost after commit+remove
    EXPECT_EQ(5, monitor->public_getReplicaMemorySyncWriteSeqno(replica));
    EXPECT_EQ(memoryAckSeqno,
              monitor->public_getReplicaMemoryAckSeqno(replica));

    // All ack'ed, committed and removed.
    try {
        monitor->seqnoAckReceived(replica, 20 /*memSeqno*/);
    } catch (const std::logic_error& e) {
        EXPECT_TRUE(std::string(e.what()).find("No tracked SyncWrite") !=
                    std::string::npos);
        return;
    }
    FAIL();
}
