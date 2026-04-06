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

#include "EdgeRouter.h"
#include "KafkaCC.h"
#include "../logger/Logger.h"
#include "../Utils.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace {
Logger& edgeRouterLogger() {
    static Logger logger;
    return logger;
}

std::string trimCopy(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();
    while (start < end && std::isspace(static_cast<unsigned char>(input[start]))) {
        start++;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        end--;
    }
    return input.substr(start, end - start);
}

std::string stripOptionalQuotes(const std::string& value) {
    std::string out = trimCopy(value);
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
        out = out.substr(1, out.size() - 2);
    }
    return trimCopy(out);
}

bool parseCsvEdgePayload(const std::string& payload, std::string& sourceId, std::string& targetId) {
    std::string line = trimCopy(payload);
    if (line.empty()) {
        return false;
    }

    size_t commaPos = line.find(',');
    if (commaPos == std::string::npos) {
        return false;
    }

    sourceId = stripOptionalQuotes(line.substr(0, commaPos));
    targetId = stripOptionalQuotes(line.substr(commaPos + 1));

    if (sourceId.empty() || targetId.empty()) {
        return false;
    }

    std::string srcLower = sourceId;
    std::string dstLower = targetId;
    std::transform(srcLower.begin(), srcLower.end(), srcLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(dstLower.begin(), dstLower.end(), dstLower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (srcLower == "source" && dstLower == "target") {
        return false;
    }

    return true;
}
}  // namespace

EdgeRouter::EdgeRouter(int graphId, const EdgeRouterConfig& config, KafkaConnector* kstream)
    : graphId_(graphId),
      config_(config),
      kstream_(kstream),
      stopRequested_(false) {
    edgeRouterLogger().info("[EdgeRouter] Initializing router for graph " + std::to_string(graphId) +
                           ", " + std::to_string(config_.workerIds.size()) + " destination workers");

    // Initialize per-worker state
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        auto conn = std::make_unique<WorkerConnection>();
        conn->workerId = config_.workerIds[i];
        conn->socketFd = -1;
        conn->connected = false;
        workerConnections_.push_back(std::move(conn));

        auto batch = std::make_unique<EdgeBatch>();
        workerBatches_.push_back(std::move(batch));

        // Initialize stats counter for each worker
        edgesPerWorker_[config_.workerIds[i]] = 0;

        edgeRouterLogger().info("[EdgeRouter] Worker " + std::to_string(i) +
                               ": id=" + std::to_string(config_.workerIds[i]) +
                               " host=" + config_.workerHosts[i] +
                               " port=" + std::to_string(config_.workerPorts[i]));
    }
}

EdgeRouter::~EdgeRouter() noexcept {
    stopRouting();
}

void EdgeRouter::startRouting() {
    edgeRouterLogger().info("[EdgeRouter] Starting Kafka consumer and routing loop");

    // Connect to all workers (blocking)
    edgeRouterLogger().info("[EdgeRouter] Establishing connections to " +
                           std::to_string(config_.workerIds.size()) + " workers");
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        connectToWorker(config_.workerIds[i], config_.workerHosts[i], config_.workerPorts[i]);
    }

    // Start batch flusher threads for each worker
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        flushThreads_.emplace_back(&EdgeRouter::batchFlushThreadFunc, this, config_.workerIds[i]);
    }

    // Start main consumer thread
    consumerThread_ = std::thread(&EdgeRouter::consumerThreadFunc, this);

    edgeRouterLogger().info("[EdgeRouter] Routing started with " + std::to_string(flushThreads_.size()) +
                           " batch flush threads");
}

void EdgeRouter::stopRouting() {
    if (stopRequested_.exchange(true)) {
        return;  // Already stopped
    }

    edgeRouterLogger().info("[EdgeRouter] Stopping router...");

    // Signal consumer thread to stop
    if (consumerThread_.joinable()) {
        consumerThread_.join();
        edgeRouterLogger().info("[EdgeRouter] Consumer thread joined");
    }

    // Stop batch flusher threads
    for (auto& thread : flushThreads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    edgeRouterLogger().info("[EdgeRouter] All worker batch threads joined");

    // Flush any remaining edges in batches
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        flushWorkerBatch(config_.workerIds[i], true);
    }

    // Disconnect from all workers
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        disconnectWorker(i);
    }

    edgeRouterLogger().info("[EdgeRouter] Router stopped. Stats: " +
                           std::to_string(totalMessagesConsumed_) + " messages consumed, " +
                           std::to_string(totalEdgesRouted_) + " edges routed");
}

EdgeRouter::Stats EdgeRouter::getStats() const {
    Stats stats;
    stats.totalMessagesConsumed = totalMessagesConsumed_;
    stats.totalEdgesRouted = totalEdgesRouted_;
    stats.totalErrorMessages = totalErrorMessages_;
    
    // Collect per-worker stats
    for (const auto& [workerId, count] : edgesPerWorker_) {
        stats.edgesPerWorker[workerId] = count.load();
    }

    return stats;
}

bool EdgeRouter::connectToWorker(int workerId, const std::string& host, int port) {
    edgeRouterLogger().info("[EdgeRouter::connectToWorker] Connecting to worker " + std::to_string(workerId) +
                           " at " + host + ":" + std::to_string(port));

    // Find the connection for this worker
    WorkerConnection* conn = nullptr;
    for (auto& c : workerConnections_) {
        if (c->workerId == workerId) {
            conn = c.get();
            break;
        }
    }

    if (!conn) {
        edgeRouterLogger().error("[EdgeRouter::connectToWorker] Worker " + std::to_string(workerId) +
                                " not found in connections list");
        return false;
    }

    // Create TCP socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        edgeRouterLogger().error("[EdgeRouter::connectToWorker] Failed to create socket for worker " +
                                std::to_string(workerId));
        return false;
    }

    // Resolve hostname
    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (status != 0) {
        edgeRouterLogger().error("[EdgeRouter::connectToWorker] Failed to resolve " + host +
                                ": " + gai_strerror(status));
        close(sockfd);
        return false;
    }

    // Attempt connection
    if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
        edgeRouterLogger().error("[EdgeRouter::connectToWorker] Failed to connect to " + host +
                                ":" + std::to_string(port) + " for worker " +
                                std::to_string(workerId));
        freeaddrinfo(res);
        close(sockfd);
        return false;
    }

    freeaddrinfo(res);

    conn->socketFd = sockfd;
    conn->connected = true;

    edgeRouterLogger().info("[EdgeRouter::connectToWorker] Successfully connected to worker " +
                           std::to_string(workerId) + " (socket=" + std::to_string(sockfd) + ")");

    return true;
}

void EdgeRouter::routeEdgeToWorker(int workerId, const json& edgeJson) {
    // Find the batch for this worker
    int workerIdx = -1;
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        if (config_.workerIds[i] == workerId) {
            workerIdx = i;
            break;
        }
    }

    if (workerIdx < 0) {
        edgeRouterLogger().error("[EdgeRouter::routeEdgeToWorker] Worker " + std::to_string(workerId) +
                                " not found");
        return;
    }

    // Add edge to this worker's batch queue
    {
        std::lock_guard<std::mutex> lock(workerBatches_[workerIdx]->lock);
        workerBatches_[workerIdx]->edges.push_back(edgeJson.dump());
    }

    totalEdgesRouted_++;
    edgesPerWorker_[workerId]++;

    // If batch size reached, signal flush (optional optimization)
    if (workerBatches_[workerIdx]->edges.size() >= static_cast<size_t>(config_.maxBatchSize)) {
        flushWorkerBatch(workerId, false);
    }
}

void EdgeRouter::flushWorkerBatch(int workerId, bool force) {
    // Find the batch for this worker
    int workerIdx = -1;
    for (size_t i = 0; i < config_.workerIds.size(); ++i) {
        if (config_.workerIds[i] == workerId) {
            workerIdx = i;
            break;
        }
    }

    if (workerIdx < 0 || workerIdx >= static_cast<int>(workerBatches_.size())) {
        return;
    }

    std::vector<std::string> edgesToSend;
    {
        std::lock_guard<std::mutex> batchLock(workerBatches_[workerIdx]->lock);
        if (workerBatches_[workerIdx]->edges.empty()) {
            return;
        }
        edgesToSend = std::move(workerBatches_[workerIdx]->edges);
        workerBatches_[workerIdx]->edges.clear();
    }

    if (edgesToSend.empty()) {
        return;
    }

    // Find connection for this worker
    WorkerConnection* conn = nullptr;
    for (auto& c : workerConnections_) {
        if (c->workerId == workerId) {
            conn = c.get();
            break;
        }
    }

    if (!conn || !conn->connected || conn->socketFd < 0) {
        edgeRouterLogger().warn("[EdgeRouter::flushWorkerBatch] Worker " + std::to_string(workerId) +
                               " not connected; discarding " + std::to_string(edgesToSend.size()) +
                               " edges");
        return;
    }

    // Send all edges in batch
    std::lock_guard<std::mutex> sockLock(conn->socketLock);
    for (const auto& edgeStr : edgesToSend) {
        json edgeJson = json::parse(edgeStr);
        sendEdgeOverSocket(conn->socketFd, edgeJson);
    }
}

void EdgeRouter::batchFlushThreadFunc(int workerId) {
    edgeRouterLogger().info("[EdgeRouter::batchFlushThreadFunc] Batch flush thread started for worker " +
                           std::to_string(workerId));

    while (!stopRequested_) {
        // Periodically flush batches (every 100ms or when batch size threshold hit in routeEdgeToWorker)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        flushWorkerBatch(workerId, false);
    }

    edgeRouterLogger().info("[EdgeRouter::batchFlushThreadFunc] Batch flush thread exiting for worker " +
                           std::to_string(workerId));
}

void EdgeRouter::consumerThreadFunc() {
    try {
        edgeRouterLogger().info("[EdgeRouter::consumerThreadFunc] Consumer thread started");

        // Create a dedicated consumer instance for the router thread.
        auto consumer = std::make_unique<cppkafka::Consumer>(kstream_->getConfig());
        consumer->subscribe({config_.topic});

        edgeRouterLogger().info("[EdgeRouter::consumerThreadFunc] Subscribed to topic '" + config_.topic +
                               "' with group ID '" + config_.groupId + "'");

        const size_t KAFKA_BATCH = 2000;
        const uint64_t MAX_IDLE_POLLS = 30;  // seconds with zero messages
        uint64_t idleSeconds = 0;

        while (!stopRequested_) {
            auto batch = consumer->poll_batch(KAFKA_BATCH, std::chrono::milliseconds(1000));

            size_t validInBatch = 0;
            for (auto& msg : batch) {
                if (!msg || msg.get_error()) {
                    ++totalErrorMessages_;
                    continue;
                }

                // Extract payload (cppkafka::Buffer in this version has no empty())
                if (!msg.get_payload()) {
                    continue;
                }
                std::string payload(msg.get_payload());

                // End-of-stream marker is produced once per Kafka partition. Do not
                // treat it as a parse failure or a routed edge.
                if (trimCopy(payload) == "-1") {
                    edgeRouterLogger().debug("[EdgeRouter::consumerThreadFunc] Received EOS marker from partition " +
                                            std::to_string(msg.get_partition()));
                    continue;
                }

                ++validInBatch;
                ++totalMessagesConsumed_;

                // Normalize to JSON edge format
                json edgeJson;
                std::string error;
                if (!normalizeKafkaPayloadToEdgeJson(payload, edgeJson, error)) {
                    edgeRouterLogger().warn("[EdgeRouter::consumerThreadFunc] Failed to parse message: " + error);
                    continue;
                }

                // Extract source node ID for hash partitioning
                std::string sourceId;
                try {
                    sourceId = std::string(edgeJson["source"]["id"]);
                } catch (...) {
                    edgeRouterLogger().warn("[EdgeRouter::consumerThreadFunc] Failed to extract source ID");
                    continue;
                }

                // Hash to partition
                int partitionId = hashPartition(sourceId);

                // Map partition to worker
                int workerIdx = getWorkerForPartition(partitionId);
                if (workerIdx < 0 || workerIdx >= static_cast<int>(config_.workerIds.size())) {
                    edgeRouterLogger().error("[EdgeRouter::consumerThreadFunc] Invalid worker index " +
                                            std::to_string(workerIdx) + " for partition " +
                                            std::to_string(partitionId));
                    continue;
                }

                int workerId = config_.workerIds[workerIdx];

                // Route to worker
                routeEdgeToWorker(workerId, edgeJson);
            }

            if (validInBatch == 0) {
                ++idleSeconds;
                if (idleSeconds % 10 == 0) {
                    edgeRouterLogger().debug("[EdgeRouter::consumerThreadFunc] Idle for " +
                                           std::to_string(idleSeconds) + " seconds (" +
                                           std::to_string(MAX_IDLE_POLLS) + "s max)");
                }
                if (idleSeconds >= MAX_IDLE_POLLS) {
                    edgeRouterLogger().info("[EdgeRouter::consumerThreadFunc] Reached max idle time, ending consumption");
                    break;
                }
            } else {
                idleSeconds = 0;  // Reset counter
            }
        }
    
        edgeRouterLogger().info("[EdgeRouter::consumerThreadFunc] Consumer thread exiting");
    } catch (const std::exception& e) {
        edgeRouterLogger().error(std::string("[EdgeRouter::consumerThreadFunc] Exception: ") + e.what());
    }
}

bool EdgeRouter::normalizeKafkaPayloadToEdgeJson(const std::string& payload, json& edgeJson, std::string& error) {
    if (config_.csvInputMode) {
        // Parse CSV: source,target
        std::string sourceId, targetId;
        if (!parseCsvEdgePayload(payload, sourceId, targetId)) {
            error = "Failed to parse CSV payload";
            return false;
        }

        edgeJson = {
            {"source", {{"id", sourceId}}},
            {"destination", {{"id", targetId}}}
        };
        return true;
    } else {
        // Parse JSON
        try {
            edgeJson = json::parse(payload);
            if (!edgeJson.contains("source") || !edgeJson.contains("destination")) {
                error = "JSON missing source or destination fields";
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            error = std::string("JSON parse failed: ") + e.what();
            return false;
        }
    }
}

bool EdgeRouter::sendEdgeOverSocket(int socketFd, const json& edgeJson) {
    if (socketFd < 0) {
        return false;
    }

    std::string edgeStr = edgeJson.dump() + "\n";
    ssize_t sent = send(socketFd, edgeStr.c_str(), edgeStr.length(), MSG_NOSIGNAL);

    if (sent < 0 || sent != static_cast<ssize_t>(edgeStr.length())) {
        edgeRouterLogger().warn("[EdgeRouter::sendEdgeOverSocket] Failed to send edge (sent=" +
                               std::to_string(sent) + "/" + std::to_string(edgeStr.length()) + ")");
        return false;
    }

    return true;
}

void EdgeRouter::disconnectWorker(int workerId) {
    WorkerConnection* conn = nullptr;
    for (auto& c : workerConnections_) {
        if (c->workerId == workerId) {
            conn = c.get();
            break;
        }
    }

    if (!conn) {
        return;
    }

    if (conn->socketFd >= 0) {
        close(conn->socketFd);
        conn->socketFd = -1;
    }
    conn->connected = false;

    edgeRouterLogger().info("[EdgeRouter::disconnectWorker] Disconnected from worker " +
                           std::to_string(workerId));
}
