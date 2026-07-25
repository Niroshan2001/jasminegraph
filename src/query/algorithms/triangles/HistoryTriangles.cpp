/**
Copyright 2026 JasminGraph Team
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
 */

#include "HistoryTriangles.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <set>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "../../../util/Utils.h"

#include <thread>

namespace {

bool parsePartitionIdFromBitmapFileName(const std::string& fileName,
                                        int graphId,
                                        uint32_t& partitionId) {
    const std::string prefix = "graph" + std::to_string(graphId) + "_part";
    const std::string suffix = "_bitmaps.ebm";

    if (fileName.rfind(prefix, 0) != 0) {
        return false;
    }
    if (fileName.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    std::string partitionText =
        fileName.substr(prefix.size(), fileName.size() - prefix.size() - suffix.size());
    if (partitionText.empty()) {
        return false;
    }
    if (!std::all_of(partitionText.begin(), partitionText.end(),
                     [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }

    partitionId = static_cast<uint32_t>(std::stoul(partitionText));
    return true;
}

bool parsePartitionIdFromDeltaFileName(const std::string& fileName,
                                       int graphId,
                                       uint32_t& partitionId) {
    const std::string prefix = "graph" + std::to_string(graphId) + "_part";
    const std::string splitMarker = "_snap";
    const std::string suffix = ".delta";

    if (fileName.rfind(prefix, 0) != 0) {
        return false;
    }
    if (fileName.size() <= prefix.size() + splitMarker.size() + suffix.size()) {
        return false;
    }
    if (fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    size_t snapPos = fileName.find(splitMarker, prefix.size());
    if (snapPos == std::string::npos || snapPos <= prefix.size()) {
        return false;
    }

    std::string partitionText = fileName.substr(prefix.size(), snapPos - prefix.size());
    if (partitionText.empty()) {
        return false;
    }
    if (!std::all_of(partitionText.begin(), partitionText.end(),
                     [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }

    partitionId = static_cast<uint32_t>(std::stoul(partitionText));
    return true;
}

std::vector<uint32_t> discoverBitmapPartitions(const std::string& snapshotDir, int graphId) {
    std::vector<std::string> files = Utils::getListOfFilesInDirectory(snapshotDir);
    std::vector<uint32_t> partitionIds;
    partitionIds.reserve(files.size());

    for (const auto& file : files) {
        uint32_t partitionId = 0;
        if (parsePartitionIdFromBitmapFileName(file, graphId, partitionId) ||
            parsePartitionIdFromDeltaFileName(file, graphId, partitionId)) {
            partitionIds.push_back(partitionId);
        }
    }

    std::sort(partitionIds.begin(), partitionIds.end());
    partitionIds.erase(std::unique(partitionIds.begin(), partitionIds.end()), partitionIds.end());
    return partitionIds;
}

std::string createEdgeShardTempDir() {
    std::string base = Utils::getJasmineGraphProperty("org.jasminegraph.server.instance.datafolder");
    if (base.empty()) {
        base = Utils::getHomeDir() + "/.jasminegraph";
    }
    if (base.empty()) {
        return "";
    }

    Utils::createDirectory(base);
    std::string templateStr = base + "/jg_histrian_edges_XXXXXX";
    std::vector<char> tempDirTemplate(templateStr.begin(), templateStr.end());
    tempDirTemplate.push_back('\0');
    char* createdDir = mkdtemp(tempDirTemplate.data());
    if (createdDir == nullptr) {
        return "";
    }
    return std::string(createdDir);
}

std::vector<uint64_t> readEncodedEdgesFromBinaryFile(const std::string& filePath) {
    std::vector<uint64_t> edges;

    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) {
        return edges;
    }

    in.seekg(0, std::ios::end);
    std::streamoff fileSize = in.tellg();
    in.seekg(0, std::ios::beg);

    if (fileSize > 0) {
        edges.reserve(static_cast<size_t>(fileSize / static_cast<std::streamoff>(sizeof(uint64_t))));
    }

    uint64_t encoded = 0;
    while (in.read(reinterpret_cast<char*>(&encoded), sizeof(encoded))) {
        edges.push_back(encoded);
    }

    return edges;
}

template <typename Handler>
void streamEncodedEdgesFromBinaryFile(const std::string& filePath, Handler&& handler) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) {
        return;
    }

    uint64_t encoded = 0;
    while (in.read(reinterpret_cast<char*>(&encoded), sizeof(encoded))) {
        handler(encoded);
    }
}

bool rewriteUniqueEncodedEdgesToBinaryFile(const std::string& filePath,
                                           const std::vector<uint64_t>& edges) {
    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    if (!edges.empty()) {
        out.write(reinterpret_cast<const char*>(edges.data()),
                  static_cast<std::streamsize>(edges.size() * sizeof(uint64_t)));
    }
    return static_cast<bool>(out);
}

void cleanupEdgeShardTempDir(const std::string& tempDir) {
    if (!tempDir.empty()) {
        Utils::deleteDirectory(tempDir);
    }
}

std::vector<TemporalStore::EdgeKey> collectCumulativeEdgesUpToSnapshot(TemporalStore& store,
                                                                        uint32_t snapshotId) {
    std::unordered_set<TemporalStore::EdgeKey, TemporalStore::EdgeKey::Hash> cumulativeEdgeSet;

    for (uint32_t sid = 0; sid <= snapshotId; sid++) {
        auto edgesAtSnapshot = store.getEdgesAtSnapshot(sid);
        cumulativeEdgeSet.insert(edgesAtSnapshot.begin(), edgesAtSnapshot.end());
    }

    return std::vector<TemporalStore::EdgeKey>(cumulativeEdgeSet.begin(), cumulativeEdgeSet.end());
}

uint64_t encodeUndirectedEdge(uint32_t sourceIndex, uint32_t destIndex) {
    if (sourceIndex > destIndex) {
        std::swap(sourceIndex, destIndex);
    }
    return (static_cast<uint64_t>(sourceIndex) << 32) | static_cast<uint64_t>(destIndex);
}

uint32_t decodeSourceIndex(uint64_t encoded) {
    return static_cast<uint32_t>(encoded >> 32);
}

uint32_t decodeDestIndex(uint64_t encoded) {
    return static_cast<uint32_t>(encoded & 0xffffffffULL);
}

size_t countCommonSortedValues(const uint32_t* left,
                               size_t leftSize,
                               const uint32_t* right,
                               size_t rightSize) {
    size_t count = 0;
    size_t leftIndex = 0;
    size_t rightIndex = 0;

    while (leftIndex < leftSize && rightIndex < rightSize) {
        uint32_t leftValue = left[leftIndex];
        uint32_t rightValue = right[rightIndex];
        if (leftValue == rightValue) {
            ++count;
            ++leftIndex;
            ++rightIndex;
        } else if (leftValue < rightValue) {
            ++leftIndex;
        } else {
            ++rightIndex;
        }
    }

    return count;
}

uint64_t countTrianglesOnCSRGraph(const std::vector<uint64_t>& csrOffsets,
                                  const std::vector<uint32_t>& csrNeighbors) {
    if (csrOffsets.size() < 2 || csrNeighbors.empty()) {
        return 0;
    }
    const uint32_t nodeCount = static_cast<uint32_t>(csrOffsets.size() - 1);
    const uint32_t* neighborsData = csrNeighbors.data();
    std::atomic<uint64_t> triangleCount(0);

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    if (nodeCount < 512) numThreads = 1;

    std::vector<std::thread> threads;
    auto worker = [&](uint32_t threadId) {
        uint64_t localCount = 0;
        for (uint32_t sourceIndex = threadId; sourceIndex < nodeCount; sourceIndex += numThreads) {
            uint64_t sourceBegin = csrOffsets[sourceIndex];
            uint64_t sourceEnd = csrOffsets[sourceIndex + 1];
            const uint32_t* sourceNeighbors = neighborsData + sourceBegin;
            size_t sourceSize = static_cast<size_t>(sourceEnd - sourceBegin);
            for (size_t middleOffset = 0; middleOffset < sourceSize; ++middleOffset) {
                uint32_t middleIndex = sourceNeighbors[middleOffset];
                uint64_t middleBegin = csrOffsets[middleIndex];
                uint64_t middleEnd = csrOffsets[middleIndex + 1];
                const uint32_t* middleNeighbors = neighborsData + middleBegin;
                size_t middleSize = static_cast<size_t>(middleEnd - middleBegin);
                if (sourceSize < middleSize) {
                    localCount += countCommonSortedValues(sourceNeighbors, sourceSize,
                                                           middleNeighbors, middleSize);
                } else {
                    localCount += countCommonSortedValues(middleNeighbors, middleSize,
                                                           sourceNeighbors, sourceSize);
                }
            }
        }
        triangleCount += localCount;
    };

    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    return triangleCount.load();
}

}  // namespace

Logger history_triangle_logger;

TemporalTriangleResult HistoryTriangles::countTrianglesAtSnapshot(
    int graphId,
    uint32_t snapshotId,
    const std::string& snapshotDir) {


    auto start = std::chrono::high_resolution_clock::now();

    // Default initialize all fields to 0
    TemporalTriangleResult result{};
    int partitionsProcessed = 0;
    std::unordered_map<std::string, uint32_t> nodeToIndex;
    uint32_t nodeCount = 0;
    uint64_t rawEdgesRead = 0;

    auto nowMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    };
    int64_t phaseStart = nowMs();

    static constexpr size_t EDGE_SHARD_COUNT = 256;
    std::string shardTempDir = createEdgeShardTempDir();
    if (shardTempDir.empty()) {
        history_triangle_logger.error("Failed to create temporary directory for edge sharding");
        return result;
    }

    std::array<std::string, EDGE_SHARD_COUNT> shardFilePaths{};
    std::array<std::ofstream, EDGE_SHARD_COUNT> shardWriters;
    static constexpr size_t SHARD_WRITE_BATCH_EDGES = 4096;
    std::array<std::vector<uint64_t>, EDGE_SHARD_COUNT> shardWriteBuffers;
    for (size_t shardId = 0; shardId < EDGE_SHARD_COUNT; ++shardId) {
        shardFilePaths[shardId] = shardTempDir + "/edges_" + std::to_string(shardId) + ".bin";
        shardWriters[shardId].open(shardFilePaths[shardId], std::ios::binary | std::ios::trunc);
        if (!shardWriters[shardId].is_open()) {
            history_triangle_logger.error("Failed to create shard file " + shardFilePaths[shardId]);
            cleanupEdgeShardTempDir(shardTempDir);
            return result;
        }
        shardWriteBuffers[shardId].reserve(SHARD_WRITE_BATCH_EDGES);
    }

    auto flushShardBuffer = [&](size_t shardId) -> bool {
        auto& buffer = shardWriteBuffers[shardId];
        if (buffer.empty()) {
            return true;
        }
        shardWriters[shardId].write(reinterpret_cast<const char*>(buffer.data()),
                                    static_cast<std::streamsize>(buffer.size() * sizeof(uint64_t)));
        if (!shardWriters[shardId].good()) {
            return false;
        }
        buffer.clear();
        return true;
    };

    auto getOrAddNode = [&](const std::string& node) -> uint32_t {
        auto it = nodeToIndex.find(node);
        if (it != nodeToIndex.end()) {
            return it->second;
        }
        uint32_t idx = nodeCount++;
        nodeToIndex.emplace(node, idx);
        return idx;
    };

    // Discover partitions from available bitmap files instead of scanning guessed IDs.
    // This supports sparse partition IDs and worker-distributed snapshot staging.
    history_triangle_logger.info("Discovering partition bitmap files for graph " +
                                 std::to_string(graphId));
    std::vector<uint32_t> partitionIds = discoverBitmapPartitions(snapshotDir, graphId);

    if (partitionIds.empty()) {
        history_triangle_logger.error("No bitmap index files found for graph " +
                                      std::to_string(graphId) + " in " + snapshotDir);
        return result;
    }

    history_triangle_logger.info("Loading cumulative edges from snapshot 0 to " +
                                 std::to_string(snapshotId) + " for " +
                                 std::to_string(partitionIds.size()) + " partition stores");

    for (uint32_t partitionId : partitionIds) {
        std::string filePath = TemporalStorePersistence::generateBitmapFilePath(
            snapshotDir, graphId, partitionId);

        bool loaded = TemporalStorePersistence::forEachActiveBitmapEdgeAtSnapshot(
            filePath, snapshotId,
            [&](const std::string& sourceId, const std::string& destId) {
                if (sourceId == destId) {
                    return true;
                }

                uint32_t sourceIndex = getOrAddNode(sourceId);
                uint32_t destIndex = getOrAddNode(destId);
                uint64_t encoded = encodeUndirectedEdge(sourceIndex, destIndex);
                size_t shardId = static_cast<size_t>(encoded & (EDGE_SHARD_COUNT - 1));
                auto& buffer = shardWriteBuffers[shardId];
                buffer.push_back(encoded);
                if (buffer.size() >= SHARD_WRITE_BATCH_EDGES) {
                    if (!flushShardBuffer(shardId)) {
                        return false;
                    }
                }
                rawEdgesRead++;
                return true;
            });

        if (loaded) {
            partitionsProcessed++;
            history_triangle_logger.info("Loaded partition " + std::to_string(partitionId) +
                                         ": streamed cumulative edges");
        } else {
            history_triangle_logger.warn("Failed to load partition bitmap file " + filePath);
        }
    }

    for (size_t shardId = 0; shardId < EDGE_SHARD_COUNT; ++shardId) {
        if (!flushShardBuffer(shardId)) {
            history_triangle_logger.error("Failed to flush shard file " + shardFilePaths[shardId]);
            cleanupEdgeShardTempDir(shardTempDir);
            return result;
        }
        shardWriters[shardId].close();
    }

    result.loadShardMs = static_cast<long>(nowMs() - phaseStart);

    if (rawEdgesRead == 0) {
        cleanupEdgeShardTempDir(shardTempDir);
        history_triangle_logger.info("No edges active at snapshot " + std::to_string(snapshotId));
        return result;
    }

    uint64_t uniqueEdgeCount = 0;

    phaseStart = nowMs();

    // Parallelize shard deduplication: each thread processes one shard independently
    std::vector<std::vector<uint64_t>> deduplicatedShards(EDGE_SHARD_COUNT);
    std::vector<uint64_t> shardCounts(EDGE_SHARD_COUNT, 0);
    static constexpr uint64_t MAX_CACHED_DEDUP_EDGES = 20000000ULL;
    std::atomic<uint64_t> cachedDedupEdges(0);
    // Deduplicate shards in parallel using std::thread
    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    std::vector<std::thread> threads;
    auto dedupWorker = [&](int threadId) {
        for (int shardId = threadId; shardId < static_cast<int>(EDGE_SHARD_COUNT); shardId += numThreads) {
            std::vector<uint64_t> shardEdges = readEncodedEdgesFromBinaryFile(shardFilePaths[shardId]);
            if (shardEdges.empty()) {
                continue;
            }

            // Sort and deduplicate in parallel (each shard independently)
            std::sort(shardEdges.begin(), shardEdges.end());
            shardEdges.erase(std::unique(shardEdges.begin(), shardEdges.end()), shardEdges.end());

            shardCounts[shardId] = shardEdges.size();

            // Always persist the deduplicated shard so we can stream from disk when RAM budget is exceeded.
            if (!rewriteUniqueEncodedEdgesToBinaryFile(shardFilePaths[shardId], shardEdges)) {
                history_triangle_logger.error("Failed to rewrite deduplicated shard file " +
                                             shardFilePaths[shardId]);
                shardCounts[shardId] = 0;
                continue;
            }

            // Cache deduplicated edges in memory only while within a safe budget.
            // This gives speedup on medium graphs and avoids OOM on very large snapshots.
            bool keepInMemory = false;
            uint64_t current = cachedDedupEdges.load(std::memory_order_relaxed);
            while (current + shardEdges.size() <= MAX_CACHED_DEDUP_EDGES) {
                if (cachedDedupEdges.compare_exchange_weak(current,
                                                           current + shardEdges.size(),
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
                    keepInMemory = true;
                    break;
                }
            }

            if (keepInMemory) {
                deduplicatedShards[shardId] = std::move(shardEdges);
            }
        }
    };
    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(dedupWorker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Accumulate edge counts from all shards
    for (uint64_t count : shardCounts) {
        uniqueEdgeCount += count;
    }

    result.cachedDedupEdges = static_cast<size_t>(cachedDedupEdges.load(std::memory_order_relaxed));

    result.dedupMs = static_cast<long>(nowMs() - phaseStart);

    phaseStart = nowMs();

    // Parallelize degree accumulation across shards (atomic updates per endpoint) using std::thread
    std::unique_ptr<std::atomic<uint32_t>[]> degree(new std::atomic<uint32_t>[nodeCount]());
    threads.clear();
    auto degreeWorker = [&](int threadId) {
        auto degreeUpdate = [&](uint64_t encoded) {
            uint32_t sourceIndex = decodeSourceIndex(encoded);
            uint32_t destIndex = decodeDestIndex(encoded);
            degree[sourceIndex]++;
            degree[destIndex]++;
        };

        for (int shardId = threadId; shardId < static_cast<int>(EDGE_SHARD_COUNT); shardId += numThreads) {
            if (!deduplicatedShards[shardId].empty()) {
                for (uint64_t encoded : deduplicatedShards[shardId]) {
                    degreeUpdate(encoded);
                }
            } else {
                streamEncodedEdgesFromBinaryFile(shardFilePaths[shardId], degreeUpdate);
            }
        }
    };
    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(degreeWorker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    result.degreeMs = static_cast<long>(nowMs() - phaseStart);

    result.rawEdges = rawEdgesRead;
    result.localEdges = uniqueEdgeCount;
    result.centralEdges = 0;
    result.partitionsProcessed = partitionsProcessed;
    result.totalEdges = uniqueEdgeCount;
    result.uniqueEdges = uniqueEdgeCount;
    result.uniqueNodes = nodeCount;

    if (partitionsProcessed == 0) {
        history_triangle_logger.error("No snapshot files found for graph " +
                                    std::to_string(graphId) + " at snapshot " +
                                    std::to_string(snapshotId));
        cleanupEdgeShardTempDir(shardTempDir);
        return result;
    }

    if (uniqueEdgeCount == 0) {
        cleanupEdgeShardTempDir(shardTempDir);
        history_triangle_logger.info("No edges active at snapshot " + std::to_string(snapshotId));
        return result;
    }

    // String-to-index mapping is not needed after this point; release memory early.
    nodeToIndex.clear();
    nodeToIndex.rehash(0);

    // Parallelize forward degree computation across shards using std::thread
    std::unique_ptr<std::atomic<uint32_t>[]> forwardDegree(new std::atomic<uint32_t>[nodeCount]());
    threads.clear();
    auto forwardWorker = [&](int threadId) {
        auto updateForwardDegree = [&](uint64_t encoded) {
            uint32_t sourceIndex = decodeSourceIndex(encoded);
            uint32_t destIndex = decodeDestIndex(encoded);

            bool sourceBeforeDest = (degree[sourceIndex] < degree[destIndex]) ||
                                    (degree[sourceIndex] == degree[destIndex] &&
                                     sourceIndex < destIndex);
            if (sourceBeforeDest) {
                forwardDegree[sourceIndex]++;
            } else {
                forwardDegree[destIndex]++;
            }
        };

        for (int shardId = threadId; shardId < static_cast<int>(EDGE_SHARD_COUNT); shardId += numThreads) {
            if (!deduplicatedShards[shardId].empty()) {
                for (uint64_t encoded : deduplicatedShards[shardId]) {
                    updateForwardDegree(encoded);
                }
            } else {
                streamEncodedEdgesFromBinaryFile(shardFilePaths[shardId], updateForwardDegree);
            }
        }
    };
    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(forwardWorker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    std::vector<uint64_t> csrOffsets(nodeCount + 1, 0);
    for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
        csrOffsets[nodeIndex + 1] = csrOffsets[nodeIndex] + forwardDegree[nodeIndex];
    }

    std::vector<uint32_t> csrNeighbors(static_cast<size_t>(csrOffsets[nodeCount]));
    std::unique_ptr<std::atomic<uint32_t>[]> forwardDegreeOffsets(new std::atomic<uint32_t>[nodeCount]());

    threads.clear();
    auto insertWorker = [&](int threadId) {
        auto insertForwardNeighbor = [&](uint64_t encoded) {
            uint32_t sourceIndex = decodeSourceIndex(encoded);
            uint32_t destIndex = decodeDestIndex(encoded);

            bool sourceBeforeDest = (degree[sourceIndex] < degree[destIndex]) ||
                                    (degree[sourceIndex] == degree[destIndex] &&
                                     sourceIndex < destIndex);
            if (sourceBeforeDest) {
                uint32_t slot = forwardDegreeOffsets[sourceIndex]++;
                csrNeighbors[static_cast<size_t>(csrOffsets[sourceIndex] + slot)] = destIndex;
            } else {
                uint32_t slot = forwardDegreeOffsets[destIndex]++;
                csrNeighbors[static_cast<size_t>(csrOffsets[destIndex] + slot)] = sourceIndex;
            }
        };

        for (int shardId = threadId; shardId < static_cast<int>(EDGE_SHARD_COUNT); shardId += numThreads) {
            if (!deduplicatedShards[shardId].empty()) {
                for (uint64_t encoded : deduplicatedShards[shardId]) {
                    insertForwardNeighbor(encoded);
                }
            } else {
                streamEncodedEdgesFromBinaryFile(shardFilePaths[shardId], insertForwardNeighbor);
            }
        }
    };
    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(insertWorker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    result.forwardBuildMs = static_cast<long>(nowMs() - phaseStart);

    degree.reset();
    forwardDegree.reset();
    forwardDegreeOffsets.reset();
    deduplicatedShards.clear();
    deduplicatedShards.shrink_to_fit();

    // Sort forward neighbor lists in parallel using std::thread
    phaseStart = nowMs();
    threads.clear();
    auto sortWorker = [&](int threadId) {
        for (int nodeIdx = threadId; nodeIdx < static_cast<int>(nodeCount); nodeIdx += numThreads) {
            uint64_t begin = csrOffsets[static_cast<size_t>(nodeIdx)];
            uint64_t end = csrOffsets[static_cast<size_t>(nodeIdx) + 1];
            if (end > begin + 1) {
                std::sort(csrNeighbors.begin() + static_cast<size_t>(begin),
                          csrNeighbors.begin() + static_cast<size_t>(end));
            }
        }
    };
    for (unsigned int i = 0; i < numThreads; ++i) {
        threads.emplace_back(sortWorker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    result.sortMs = static_cast<long>(nowMs() - phaseStart);

    cleanupEdgeShardTempDir(shardTempDir);

    history_triangle_logger.info("Counting triangles on CSR forward graph with " +
                                 std::to_string(result.uniqueEdges) + " edges");

    phaseStart = nowMs();
    result.triangleCount = countTrianglesOnCSRGraph(csrOffsets, csrNeighbors);
    result.countMs = static_cast<long>(nowMs() - phaseStart);

    auto end = std::chrono::high_resolution_clock::now();
    result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    history_triangle_logger.info("Triangle count completed: " + std::to_string(result.triangleCount) +
                               " triangles found in " + std::to_string(result.durationMs) + "ms");

    return result;
}


