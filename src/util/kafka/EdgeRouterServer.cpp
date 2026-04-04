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
**/

#include "EdgeRouterServer.h"
#include "../logger/Logger.h"
#include "../../temporalstore/TemporalStore.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace {
Logger& edgeRouterServerLogger() {
    static Logger logger;
    return logger;
}
}  // namespace

EdgeRouterServer::EdgeRouterServer(const EdgeRouterServerConfig& config,
                                  std::map<int, std::unique_ptr<TemporalStore>>& localTemporalStores,
                                                                    std::unique_ptr<TemporalStore>& centralTemporalStore)
    : config_(config),
      stopRequested_(false),
      listenSocketFd_(-1),
      localTemporalStores_(localTemporalStores),
      centralTemporalStore_(centralTemporalStore),
      globalSnapshotId_(config.initialSnapshotId) {
    
    edgeRouterServerLogger().info("[EdgeRouterServer] Initialized for worker listening on port " +
                                  std::to_string(config_.port));

    // Initialize per-partition mutexes
    for (int i = 0; i < config_.numberOfPartitions; ++i) {
        partitionMutexes_.push_back(std::make_unique<std::mutex>());
        partitionTemporalMutexes_.push_back(std::make_unique<std::mutex>());
    }
}

EdgeRouterServer::~EdgeRouterServer() noexcept {
    stop();
}

void EdgeRouterServer::start() {
    edgeRouterServerLogger().info("[EdgeRouterServer] Starting router server on port " +
                                 std::to_string(config_.port));

    // Create listening socket
    listenSocketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocketFd_ < 0) {
        edgeRouterServerLogger().error("[EdgeRouterServer] Failed to create socket");
        return;
    }

    // Allow socket reuse
    int opt = 1;
    if (setsockopt(listenSocketFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        edgeRouterServerLogger().error("[EdgeRouterServer] Failed to set SO_REUSEADDR");
        close(listenSocketFd_);
        listenSocketFd_ = -1;
        return;
    }

    // Bind socket
    struct sockaddr_in serverAddr;
    std::memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(config_.port);

    if (bind(listenSocketFd_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        edgeRouterServerLogger().error("[EdgeRouterServer] Failed to bind to port " +
                                       std::to_string(config_.port));
        close(listenSocketFd_);
        listenSocketFd_ = -1;
        return;
    }

    // Listen for connections
    if (listen(listenSocketFd_, 32) < 0) {
        edgeRouterServerLogger().error("[EdgeRouterServer] Failed to listen");
        close(listenSocketFd_);
        listenSocketFd_ = -1;
        return;
    }

    edgeRouterServerLogger().info("[EdgeRouterServer] Listening on port " +
                                 std::to_string(config_.port));

    // Start accept thread
    acceptThread_ = std::thread(&EdgeRouterServer::acceptThreadFunc, this);

    edgeRouterServerLogger().info("[EdgeRouterServer] Router server started");
}

void EdgeRouterServer::stop() {
    if (stopRequested_.exchange(true)) {
        return;  // Already stopped
    }

    edgeRouterServerLogger().info("[EdgeRouterServer] Stopping router server...");

    // Close listening socket to stop accept loop
    if (listenSocketFd_ >= 0) {
        close(listenSocketFd_);
        listenSocketFd_ = -1;
    }

    // Join accept thread
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // Join all processor threads
    for (auto& thread : processorThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    edgeRouterServerLogger().info("[EdgeRouterServer] Router server stopped. Stats: " +
                                 std::to_string(totalEdgesReceived_) + " edges received");
}

EdgeRouterServer::Stats EdgeRouterServer::getStats() const {
    Stats stats;
    stats.totalEdgesReceived = totalEdgesReceived_;
    stats.totalLocalEdges = totalLocalEdges_;
    stats.totalCentralEdges = totalCentralEdges_;
    stats.totalErrorEdges = totalErrorEdges_;
    return stats;
}

void EdgeRouterServer::acceptThreadFunc() {
    edgeRouterServerLogger().info("[EdgeRouterServer::acceptThreadFunc] Accept thread started");

    while (!stopRequested_ && listenSocketFd_ >= 0) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientSocketFd = accept(listenSocketFd_, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSocketFd < 0) {
            if (!stopRequested_) {
                edgeRouterServerLogger().warn("[EdgeRouterServer::acceptThreadFunc] Accept failed");
            }
            break;
        }

        edgeRouterServerLogger().debug("[EdgeRouterServer::acceptThreadFunc] New client connection");

        // Spawn processor thread for this client
        processorThreads_.emplace_back(&EdgeRouterServer::processClientThreadFunc, this, clientSocketFd);
    }

    edgeRouterServerLogger().info("[EdgeRouterServer::acceptThreadFunc] Accept thread exiting");
}

void EdgeRouterServer::processClientThreadFunc(int clientSocketFd) {
    try {
        edgeRouterServerLogger().debug("[EdgeRouterServer::processClientThreadFunc] Processing client connection");

        std::string line;
        while (!stopRequested_ && readJsonLineFromSocket(clientSocketFd, line)) {
            if (line.empty()) {
                continue;
            }

            ++totalEdgesReceived_;

            // Parse edge JSON
            try {
                json edgeJson = json::parse(line);
                storeEdge(edgeJson);
                sendAckToRouter(clientSocketFd);
            } catch (const std::exception& e) {
                ++totalErrorEdges_;
                edgeRouterServerLogger().warn(std::string("[EdgeRouterServer::processClientThreadFunc] Parse error: ") +
                                             e.what());
            }
        }

        close(clientSocketFd);
        edgeRouterServerLogger().debug("[EdgeRouterServer::processClientThreadFunc] Client connection closed");
    } catch (const std::exception& e) {
        edgeRouterServerLogger().error(std::string("[EdgeRouterServer::processClientThreadFunc] Exception: ") +
                                      e.what());
        close(clientSocketFd);
    }
}

bool EdgeRouterServer::isOwnedPartition(int partitionId) const {
    return std::find(config_.ownedPartitions.begin(), config_.ownedPartitions.end(),
                    partitionId) != config_.ownedPartitions.end();
}

void EdgeRouterServer::storeEdge(const json& edgeJson) {
    try {
        // Extract partition IDs
        // Note: EdgeRouter pre-computes these, so we can use them directly
        int part_s = -1, part_d = -1;
        
        try {
            part_s = edgeJson["source"]["pid"].get<int>();
            part_d = edgeJson["destination"]["pid"].get<int>();
        } catch (...) {
            // If partition IDs not pre-computed, compute them (shouldn't happen with router)
            std::hash<std::string> hasher;
            std::string sourceId = edgeJson["source"]["id"].get<std::string>();
            std::string destId = edgeJson["destination"]["id"].get<std::string>();
            part_s = static_cast<int>(hasher(sourceId) % config_.numberOfPartitions);
            part_d = static_cast<int>(hasher(destId) % config_.numberOfPartitions);
        }

        // Verify we own at least one side (should always be true with router)
        bool srcOwned = isOwnedPartition(part_s);
        bool dstOwned = isOwnedPartition(part_d);

        if (!srcOwned && !dstOwned) {
            edgeRouterServerLogger().warn("[EdgeRouterServer::storeEdge] Received edge neither endpoint is owned");
            ++totalErrorEdges_;
            return;
        }

        // ---- TEMPORAL STORE ----
        if (config_.temporalEnabled) {
            bool shouldSnapshot = false;
            
            if (part_s == part_d) {
                // Local edge
                auto it = localTemporalStores_.find(part_s);
                if (it != localTemporalStores_.end()) {
                    std::lock_guard<std::mutex> tlock(*partitionTemporalMutexes_[part_s]);
                    std::string sourceId = edgeJson["source"]["id"].get<std::string>();
                    std::string destId = edgeJson["destination"]["id"].get<std::string>();
                    it->second->addEdge(sourceId, destId, globalSnapshotId_.load());
                    ++totalLocalEdges_;
                    if (it->second->shouldCreateSnapshot()) {
                        shouldSnapshot = true;
                    }
                }
            } else {
                // Central edge
                if (centralTemporalStore_) {
                    std::lock_guard<std::mutex> tlock(snapshotMutex_);
                    std::string sourceId = edgeJson["source"]["id"].get<std::string>();
                    std::string destId = edgeJson["destination"]["id"].get<std::string>();
                    centralTemporalStore_->addEdge(sourceId, destId, globalSnapshotId_.load());
                    ++totalCentralEdges_;
                    if (centralTemporalStore_->shouldCreateSnapshot()) {
                        shouldSnapshot = true;
                    }
                }
            }

            // Attempt to create snapshot (CAS pattern)
            if (shouldSnapshot) {
                bool expected = false;
                if (snapshotInProgress_.compare_exchange_strong(
                        expected, true,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    createGlobalSnapshot();
                    snapshotInProgress_.store(false, std::memory_order_release);
                }
            }
        } else {
            // Count edge type for statistics
            if (part_s == part_d) {
                ++totalLocalEdges_;
            } else {
                ++totalCentralEdges_;
            }
        }

        // Incremental store integration is intentionally deferred until the
        // router server is wired to worker-local incremental stores.

    } catch (const std::exception& e) {
        ++totalErrorEdges_;
        edgeRouterServerLogger().error(std::string("[EdgeRouterServer::storeEdge] Error: ") + e.what());
    }
}

void EdgeRouterServer::createGlobalSnapshot() {
    edgeRouterServerLogger().info("[EdgeRouterServer::createGlobalSnapshot] Creating global snapshot");
    
    globalSnapshotId_++;

    // Trigger snapshots on all temporal stores
    for (auto& [partId, store] : localTemporalStores_) {
        if (store) {
            store->openNewSnapshot();
        }
    }
    if (centralTemporalStore_) {
        centralTemporalStore_->openNewSnapshot();
    }
}

void EdgeRouterServer::sendAckToRouter(int clientSocketFd) {
    // Send simple ack (optional, for flow control)
    const char* ack = "OK\n";
    send(clientSocketFd, ack, strlen(ack), MSG_NOSIGNAL);
}

bool EdgeRouterServer::readJsonLineFromSocket(int socketFd, std::string& line) {
    line.clear();
    char buffer[8192];
    
    while (true) {
        ssize_t bytesRead = recv(socketFd, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) {
            return false;  // Connection closed or error
        }

        for (ssize_t i = 0; i < bytesRead; ++i) {
            if (buffer[i] == '\n') {
                // Found end of line
                return true;
            }
            line += buffer[i];
        }

        // Prevent buffer overflow
        if (line.size() > 1000000) {
            edgeRouterServerLogger().error("[EdgeRouterServer::readJsonLineFromSocket] Line too long");
            return false;
        }
    }
}
