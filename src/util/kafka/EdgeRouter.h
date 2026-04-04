/**
Copyright 2023 JasmineGraph Team
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Edge Router — Dedicated Kafka consumer that routes edges to workers based on hash partitioning.
- Single consumer group (reads all Kafka messages once)
- Deterministic hash partitioning assigns edges to workers
- Routes via TCP sockets to each worker's router server
- Reduces network bandwidth by 50% vs. fan-out model
**/

#ifndef JASMINEGRAPH_EDGEROUTER_H
#define JASMINEGRAPH_EDGEROUTER_H

#include <cppkafka/cppkafka.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <memory>

class KafkaConnector;

using json = nlohmann::json;

/**
 * Router configuration passed from master/StreamHandler to the router.
 */
struct EdgeRouterConfig {
    std::string brokers;
    std::string topic;
    std::string groupId;
    int graphId;
    int numberOfPartitions;
    std::vector<int> workerIds;  // List of destination worker IDs
    std::vector<std::string> workerHosts;  // Host IP for each worker
    std::vector<int> workerPorts;          // Router server port for each worker
    bool csvInputMode;
    int maxBatchSize;  // Route edges in batches for efficiency
    uint32_t initialSnapshotId;
};

/**
 * EdgeRouter — Master-side component that:
 *   1. Consumes from Kafka topic (single consumer group = all edges read once)
 *   2. Hashes source node ID to determine partition
 *   3. Maps partition to destination worker
 *   4. Routes edge to that worker's router server via TCP
 */
class EdgeRouter {
 public:
    EdgeRouter(int graphId, const EdgeRouterConfig& config, KafkaConnector* kstream);
    ~EdgeRouter() noexcept;

    // Start consuming from Kafka and routing to workers
    void startRouting();

    // Stop the router gracefully
    void stopRouting();

    // Get statistics
    struct Stats {
        uint64_t totalMessagesConsumed;
        uint64_t totalEdgesRouted;
        uint64_t totalErrorMessages;
        std::unordered_map<int, uint64_t> edgesPerWorker;
    };
    Stats getStats() const;

 private:
    int graphId_;
    EdgeRouterConfig config_;
    KafkaConnector* kstream_;
    std::atomic<bool> stopRequested_;

    // Statistics
    std::atomic<uint64_t> totalMessagesConsumed_{0};
    std::atomic<uint64_t> totalEdgesRouted_{0};
    std::atomic<uint64_t> totalErrorMessages_{0};
    std::unordered_map<int, std::atomic<uint64_t>> edgesPerWorker_;
    std::mutex statsLock_;

    // Per-worker TCP connections (pooled)
    struct WorkerConnection {
        int workerId;
        int socketFd;
        std::mutex socketLock;
        bool connected;
    };
    std::vector<std::unique_ptr<WorkerConnection>> workerConnections_;

    // Batching queues per worker (accumulate edges before flushing to reduce TCP overhead)
    struct EdgeBatch {
        std::vector<std::string> edges;  // JSON-serialized edges
        std::mutex lock;
    };
    std::vector<std::unique_ptr<EdgeBatch>> workerBatches_;
    std::vector<std::thread> flushThreads_;

    // Main consumer thread
    std::thread consumerThread_;

    // ---- Helper Methods ----

    // Establish TCP connection to worker's router server
    bool connectToWorker(int workerId, const std::string& host, int port);

    // Send edge to worker via batching queue
    void routeEdgeToWorker(int workerId, const json& edgeJson);

    // Flush accumulated edges to worker
    void flushWorkerBatch(int workerId, bool force = false);

    // Deterministic partition assignment: same hash formula as Partitioner
    int hashPartition(const std::string& nodeId) const {
        std::hash<std::string> hasher;
        return static_cast<int>(hasher(nodeId) % config_.numberOfPartitions);
    }

    // Map partition to worker: partition % numberOfWorkers = workerIndex
    int getWorkerForPartition(int partitionId) const {
        return partitionId % config_.workerIds.size();
    }

    // Main consume loop
    void consumerThreadFunc();

    // Worker batch flusher thread
    void batchFlushThreadFunc(int workerId);

    // Converts Kafka payload (JSON or CSV) to normalized edge JSON
    bool normalizeKafkaPayloadToEdgeJson(const std::string& payload, json& edgeJson, std::string& error);

    // Send JSON edge over TCP socket
    bool sendEdgeOverSocket(int socketFd, const json& edgeJson);

    // Disconnect and cleanup worker connection
    void disconnectWorker(int workerId);
};

#endif  // JASMINEGRAPH_EDGEROUTER_H
