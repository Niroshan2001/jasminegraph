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

Edge Router Server — Worker-side component that listens for routed edges from EdgeRouter.
- Listens on TCP socket for incoming edges
- Edges come pre-partitioned (no hash computation needed)
- Directly stores edges to appropriate local/central stores
- Bypasses edge filtering (all edges received are relevant)
**/

#ifndef JASMINEGRAPH_EDGEROUTERSERVER_H
#define JASMINEGRAPH_EDGEROUTERSERVER_H

#include <string>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * Router server configuration (worker-side)
 */
struct EdgeRouterServerConfig {
    int port;                           // Port to listen on
    int graphId;
    int numberOfPartitions;
    std::vector<int> ownedPartitions;   // Partitions owned by this worker
    bool temporalEnabled;
    uint64_t timeThreshold;
    uint64_t edgeThreshold;
    uint32_t initialSnapshotId;
};

class TemporalStore;  // Forward declaration

/**
 * EdgeRouterServer — Worker-side component that:
 *   1. Listens on TCP socket for edges from EdgeRouter
 *   2. Receives pre-partitioned edges (no filtering needed)
 *   3. Stores edges to local/central stores directly
 *   4. Handles temporal snapshots per partition
 */
class EdgeRouterServer {
 public:
    EdgeRouterServer(const EdgeRouterServerConfig& config, 
                    std::map<int, std::unique_ptr<TemporalStore>>& localTemporalStores,
                          std::unique_ptr<TemporalStore>& centralTemporalStore);
    
    ~EdgeRouterServer() noexcept;

    // Start listening for edges
    void start();

    // Stop gracefully
    void stop();

    // Get statistics
    struct Stats {
        uint64_t totalEdgesReceived;
        uint64_t totalLocalEdges;
        uint64_t totalCentralEdges;
        uint64_t totalErrorEdges;
    };
    Stats getStats() const;

 private:
    EdgeRouterServerConfig config_;
    std::atomic<bool> stopRequested_;
    int listenSocketFd_;
    std::thread acceptThread_;
    std::vector<std::thread> processorThreads_;

    // References to worker stores (shared with main worker consumer)
    std::map<int, std::unique_ptr<TemporalStore>>& localTemporalStores_;
    std::unique_ptr<TemporalStore>& centralTemporalStore_;
    std::atomic<uint32_t> globalSnapshotId_;

    // Statistics
    std::atomic<uint64_t> totalEdgesReceived_{0};
    std::atomic<uint64_t> totalLocalEdges_{0};
    std::atomic<uint64_t> totalCentralEdges_{0};
    std::atomic<uint64_t> totalErrorEdges_{0};

    // Synchronization for snapshot creation
    std::atomic<bool> snapshotInProgress_{false};
    std::mutex snapshotMutex_;
    std::vector<std::unique_ptr<std::mutex>> partitionTemporalMutexes_;

    // Per-partition store locks (for reduced lock contention)
    std::vector<std::unique_ptr<std::mutex>> partitionMutexes_;

    // ---- Helper Methods ----

    // Listen for incoming client connections
    void acceptThreadFunc();

    // Process incoming edges from a client socket
    void processClientThreadFunc(int clientSocketFd);

    // Check if partition is owned by this worker
    bool isOwnedPartition(int partitionId) const;

    // Store edge to local or central store
    void storeEdge(const json& edgeJson);

    // Create snapshot (triggered when threshold reached)
    void createGlobalSnapshot();

    // Send HTTP-like response back to client
    void sendAckToRouter(int clientSocketFd);

    // Read line-buffered JSON from socket
    bool readJsonLineFromSocket(int socketFd, std::string& line);
};

#endif  // JASMINEGRAPH_EDGEROUTERSERVER_H
