/**
 * DeltaEncodedTemporalStore.h
 * 
 * Optimized temporal storage with:
 * - Delta encoding (snapshot chains limited to 10)
 * - Per-snapshot edge lists (column-wise indexing)
 * - Lazy materialization with caching
 * - Batch insert support for high-throughput streaming
 */

#ifndef DELTA_ENCODED_TEMPORAL_STORE_H
#define DELTA_ENCODED_TEMPORAL_STORE_H

#include <roaring/roaring.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "PropertyIntervalDictionary.h"

class DeltaEncodedTemporalStore {
public:
    struct SnapshotData {
        uint32_t snapshotId;
        uint64_t timestamp;
        uint32_t baseSnapshotId;  // For delta encoding
        bool isBase;              // True if this is a full snapshot (every 10th)
        
        // Delta storage: Only changes from base
        roaring_bitmap_t* addedEdges;    // New edges vs base
        roaring_bitmap_t* removedEdges;  // Deleted edges vs base
        
        // Full storage (for base snapshots only)
        roaring_bitmap_t* fullEdgeBitmap;  // All edges if isBase=true
        
        // Materialized cache (lazy)
        roaring_bitmap_t* materializedCache;  // Reconstructed full bitmap
        bool isCached;
        
        uint64_t edgeCount;
        
        SnapshotData(uint32_t id, bool base = false) 
            : snapshotId(id), timestamp(0), baseSnapshotId(id),
              isBase(base), isCached(false), edgeCount(0) {
            addedEdges = roaring_bitmap_create();
            removedEdges = roaring_bitmap_create();
            fullEdgeBitmap = base ? roaring_bitmap_create() : nullptr;
            materializedCache = nullptr;
        }
        
        ~SnapshotData() {
            roaring_bitmap_free(addedEdges);
            roaring_bitmap_free(removedEdges);
            if (fullEdgeBitmap) roaring_bitmap_free(fullEdgeBitmap);
            if (materializedCache) roaring_bitmap_free(materializedCache);
        }
    };

private:
    uint32_t graphId_;
    uint32_t partitionId_;
    uint32_t currentSnapshotId_;
    
    // Edge catalog: String node IDs → Integer edge index
    std::vector<std::pair<std::string, std::string>> edgeList_;  // edgeIndex → (srcId, dstId)
    std::map<std::string, size_t> edgeMap_;                      // "src->dst" → edgeIndex
    
    // Snapshot storage
    std::map<uint32_t, std::unique_ptr<SnapshotData>> snapshots_;
    SnapshotData* currentSnapshot_;
    
    // Delta chain configuration
    static constexpr int DELTA_CHAIN_LENGTH = 10;  // Full snapshot every 10 deltas
    
    // Thread safety
    mutable std::mutex mutex_;
    
    // Statistics
    uint64_t totalEdgesAdded_;
    uint64_t totalEdgesRemoved_;
    
    // Property storage
    std::map<std::string, PropertyIntervalDictionary> nodeProperties_;  // nodeId -> properties
    std::map<std::string, PropertyIntervalDictionary> edgeProperties_;  // "src->dst" -> properties
    
    // Helper: Get or create edge index
    size_t getOrCreateEdgeIndex(const std::string& src, const std::string& dst) {
        std::string edgeKey = src + "->" + dst;
        auto it = edgeMap_.find(edgeKey);
        if (it != edgeMap_.end()) {
            return it->second;
        }
        
        size_t newIndex = edgeList_.size();
        edgeList_.push_back({src, dst});
        edgeMap_[edgeKey] = newIndex;
        return newIndex;
    }
    
    // Helper: Reconstruct snapshot from delta chain
    roaring_bitmap_t* reconstructFromDeltas(uint32_t snapshotId) {
        auto snapshot = snapshots_.find(snapshotId);
        if (snapshot == snapshots_.end()) {
            return nullptr;
        }
        
        SnapshotData* snap = snapshot->second.get();
        
        // If it's a base snapshot, return full bitmap directly
        if (snap->isBase) {
            return roaring_bitmap_copy(snap->fullEdgeBitmap);
        }
        
        // Otherwise, reconstruct from base + deltas
        uint32_t baseId = snap->baseSnapshotId;
        auto baseSnapshot = snapshots_.find(baseId);
        if (baseSnapshot == snapshots_.end()) {
            return nullptr;
        }
        
        roaring_bitmap_t* result = roaring_bitmap_copy(baseSnapshot->second->fullEdgeBitmap);
        
        // Apply all intermediate deltas from base to current
        for (uint32_t sid = baseId + 1; sid <= snapshotId; sid++) {
            auto intermediateSnap = snapshots_.find(sid);
            if (intermediateSnap == snapshots_.end()) continue;
            
            SnapshotData* iSnap = intermediateSnap->second.get();
            roaring_bitmap_or_inplace(result, iSnap->addedEdges);
            roaring_bitmap_andnot_inplace(result, iSnap->removedEdges);
        }
        
        return result;
    }

public:
    DeltaEncodedTemporalStore(uint32_t graphId, uint32_t partitionId)
        : graphId_(graphId),
          partitionId_(partitionId),
          currentSnapshotId_(0),
          currentSnapshot_(nullptr),
          totalEdgesAdded_(0),
          totalEdgesRemoved_(0) {
        
        // Create initial base snapshot
        openNewSnapshot();
    }
    
    ~DeltaEncodedTemporalStore() = default;
    
    /**
     * Open a new snapshot
     */
    uint32_t openNewSnapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        currentSnapshotId_++;
        
        // Every DELTA_CHAIN_LENGTH snapshot is a full base
        bool isBase = (currentSnapshotId_ % DELTA_CHAIN_LENGTH) == 0;
        
        auto newSnapshot = std::make_unique<SnapshotData>(currentSnapshotId_, isBase);
        newSnapshot->timestamp = std::time(nullptr);
        
        if (isBase) {
            // For base snapshots, materialize the full edge set from previous snapshot
            if (currentSnapshot_ != nullptr) {
                roaring_bitmap_t* previousFull = getSnapshotEdges(currentSnapshotId_ - 1);
                if (previousFull) {
                    roaring_bitmap_free(newSnapshot->fullEdgeBitmap);
                    newSnapshot->fullEdgeBitmap = roaring_bitmap_copy(previousFull);
                    roaring_bitmap_free(previousFull);
                }
            }
            newSnapshot->baseSnapshotId = currentSnapshotId_;
        } else {
            // For delta snapshots, reference the nearest base
            uint32_t baseId = (currentSnapshotId_ / DELTA_CHAIN_LENGTH) * DELTA_CHAIN_LENGTH;
            if (baseId == 0) baseId = DELTA_CHAIN_LENGTH;  // Handle first chain
            newSnapshot->baseSnapshotId = baseId;
        }
        
        currentSnapshot_ = newSnapshot.get();
        snapshots_[currentSnapshotId_] = std::move(newSnapshot);
        
        return currentSnapshotId_;
    }
    
    /**
     * Add edge to current snapshot (single edge)
     */
    void addEdge(const std::string& src, const std::string& dst) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (currentSnapshot_ == nullptr) {
            openNewSnapshot();
        }
        
        size_t edgeIndex = getOrCreateEdgeIndex(src, dst);
        
        if (currentSnapshot_->isBase) {
            // Base snapshot: Add to full bitmap
            roaring_bitmap_add(currentSnapshot_->fullEdgeBitmap, edgeIndex);
        } else {
            // Delta snapshot: Add to addedEdges, remove from removedEdges
            roaring_bitmap_add(currentSnapshot_->addedEdges, edgeIndex);
            roaring_bitmap_remove(currentSnapshot_->removedEdges, edgeIndex);
        }
        
        currentSnapshot_->edgeCount++;
        totalEdgesAdded_++;
        
        // Invalidate cache
        if (currentSnapshot_->materializedCache) {
            roaring_bitmap_free(currentSnapshot_->materializedCache);
            currentSnapshot_->materializedCache = nullptr;
            currentSnapshot_->isCached = false;
        }
    }
    
    /**
     * Add edges in batch (high-throughput optimization)
     */
    void addEdgeBatch(const std::vector<std::pair<std::string, std::string>>& edges) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (currentSnapshot_ == nullptr) {
            openNewSnapshot();
        }
        
        for (const auto& edge : edges) {
            size_t edgeIndex = getOrCreateEdgeIndex(edge.first, edge.second);
            
            if (currentSnapshot_->isBase) {
                roaring_bitmap_add(currentSnapshot_->fullEdgeBitmap, edgeIndex);
            } else {
                roaring_bitmap_add(currentSnapshot_->addedEdges, edgeIndex);
                roaring_bitmap_remove(currentSnapshot_->removedEdges, edgeIndex);
            }
            
            currentSnapshot_->edgeCount++;
            totalEdgesAdded_++;
        }
        
        // Optimize bitmaps once at end (better compression)
        if (currentSnapshot_->isBase) {
            roaring_bitmap_run_optimize(currentSnapshot_->fullEdgeBitmap);
        } else {
            roaring_bitmap_run_optimize(currentSnapshot_->addedEdges);
        }
        
        // Invalidate cache
        if (currentSnapshot_->materializedCache) {
            roaring_bitmap_free(currentSnapshot_->materializedCache);
            currentSnapshot_->materializedCache = nullptr;
            currentSnapshot_->isCached = false;
       Remove edge from current snapshot (single edge)
     */
    void removeEdge(const std::string& src, const std::string& dst) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (currentSnapshot_ == nullptr) {
            openNewSnapshot();
        }
        
        std::string edgeKey = src + "->" + dst;
        auto it = edgeMap_.find(edgeKey);
        if (it == edgeMap_.end()) {
            // Edge doesn't exist in catalog, nothing to remove
            return;
        }
        
        size_t edgeIndex = it->second;
        
        if (currentSnapshot_->isBase) {
            // Base snapshot: Remove from full bitmap
            roaring_bitmap_remove(currentSnapshot_->fullEdgeBitmap, edgeIndex);
        } else {
            // Delta snapshot: Add to removedEdges, remove from addedEdges
            roaring_bitmap_add(currentSnapshot_->removedEdges, edgeIndex);
            roaring_bitmap_remove(currentSnapshot_->addedEdges, edgeIndex);
        }
        
        if (currentSnapshot_->edgeCount > 0) {
            currentSnapshot_->edgeCount--;
        }
        totalEdgesRemoved_++;
        
        // Invalidate cache
        if (currentSnapshot_->materializedCache) {
            roaring_bitmap_free(currentSnapshot_->materializedCache);
            currentSnapshot_->materializedCache = nullptr;
            currentSnapshot_->isCached = false;
        }
    }
    
    /**
     * Remove edges in batch (high-throughput optimization)
     */
    void removeEdgeBatch(const std::vector<std::pair<std::string, std::string>>& edges) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (currentSnapshot_ == nullptr) {
            openNewSnapshot();
        }
        
        for (const auto& edge : edges) {
            std::string edgeKey = edge.first + "->" + edge.second;
            auto it = edgeMap_.find(edgeKey);
            if (it == edgeMap_.end()) {
                continue;  // Edge not in catalog
            }
            
            size_t edgeIndex = it->second;
            
            if (currentSnapshot_->isBase) {
                roaring_bitmap_remove(currentSnapshot_->fullEdgeBitmap, edgeIndex);
            } else {
                roaring_bitmap_add(currentSnapshot_->removedEdges, edgeIndex);
                roaring_bitmap_remove(currentSnapshot_->addedEdges, edgeIndex);
            }
            
            if (currentSnapshot_->edgeCount > 0) {
                currentSnapshot_->edgeCount--;
            }
            totalEdgesRemoved_++;
        }
        
        // Optimize bitmaps once at end
        if (!currentSnapshot_->isBase) {
            roaring_bitmap_run_optimize(currentSnapshot_->removedEdges);
        }
        
        // Invalidate cache
        if (currentSnapshot_->materializedCache) {
            roaring_bitmap_free(currentSnapshot_->materializedCache);
            currentSnapshot_->materializedCache = nullptr;
            currentSnapshot_->isCached = false;
        }
    }
    
    /**
     * Update node property at current snapshot
     */
    void updateNodeProperty(const std::string& nodeId,
                           const std::string& propertyKey,
                           const std::string& propertyValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        uint32_t snapshotId = currentSnapshotId_;
        auto& propDict = nodeProperties_[nodeId];
        propDict.addOrUpdateProperty(propertyKey, propertyValue, snapshotId);
    }
    
    /**
     * Update edge property at current snapshot
     */
    void updateEdgeProperty(const std::string& src,
                           const std::string& dst,
                           const std::string& propertyKey,
                           const std::string& propertyValue) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        uint32_t snapshotId = currentSnapshotId_;
        std::string edgeKey = src + "->" + dst;
        auto& propDict = edgeProperties_[edgeKey];
        propDict.addOrUpdateProperty(propertyKey, propertyValue, snapshotId);
    }
    
    /**
     * Get node property value at specific snapshot
     */
    std::string getNodePropertyAtSnapshot(const std::string& nodeId,
                                         const std::string& propertyKey,
                                         uint32_t snapshotId) const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        
        auto it = nodeProperties_.find(nodeId);
        if (it != nodeProperties_.end()) {
            return it->second.getValueAtSnapshot(propertyKey, snapshotId);
        }
        return "";
    }
    
    /**
     * Get edge property value at specific snapshot
     */
    std::string getEdgePropertyAtSnapshot(const std::string& src,
                                         const std::string& dst,
                                         const std::string& propertyKey,
                                         uint32_t snapshotId) const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        
        std::string edgeKey = src + "->" + dst;
        auto it = edgeProperties_.find(edgeKey);
        if (it != edgeProperties_.end()) {
            return it->second.getValueAtSnapshot(propertyKey, snapshotId);
        }
        return "";
    }
    
    /**
     * Check if edge exists at specific snapshot
     */
    bool edgeExistsAtSnapshot(const std::string& src, const std::string& dst, uint32_t snapshotId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::string edgeKey = src + "->" + dst;
        auto it = edgeMap_.find(edgeKey);
        if (it == edgeMap_.end()) {
            return false;
        }
        
        size_t edgeIndex = it->second;
        roaring_bitmap_t* snapshotBitmap = getSnapshotEdges(snapshotId);
        if (snapshotBitmap == nullptr) {
            return false;
        }
        
        bool exists = roaring_bitmap_contains(snapshotBitmap, edgeIndex);
        roaring_bitmap_free(snapshotBitmap);
        return exists;
    }
    
    /**
     *  }
    }
    
    /**
     * Get edges at a specific snapshot (with lazy materialization)
     */
    roaring_bitmap_t* getSnapshotEdges(uint32_t snapshotId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = snapshots_.find(snapshotId);
        if (it == snapshots_.end()) {
            return nullptr;
        }
        
        SnapshotData* snapshot = it->second.get();
        
        // If cached, return cached copy
        if (snapshot->isCached && snapshot->materializedCache) {
            return roaring_bitmap_copy(snapshot->materializedCache);
        }
        
        // If base snapshot, return full bitmap
        if (snapshot->isBase) {
            return roaring_bitmap_copy(snapshot->fullEdgeBitmap);
        }
        
        // Otherwise, reconstruct from deltas
        roaring_bitmap_t* reconstructed = reconstructFromDeltas(snapshotId);
        
        // Cache the result
        if (reconstructed) {
            snapshot->materializedCache = roaring_bitmap_copy(reconstructed);
            snapshot->isCached = true;
        }
        
        return reconstructed;
    }
    
    /**
     * Get current snapshot ID
     */
    uint32_t getCurrentSnapshotId() const {
        return currentSnapshotId_;
    }
    
    /**
     * Get edge count at snapshot
     */
    uint64_t getEdgeCountAtSnapshot(uint32_t snapshotId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = snapshots_.find(snapshotId);
        if (it == snapshots_.end()) {
            return 0;
        }
        
        // For accurate count, get the full bitmap
        roaring_bitmap_t* fullBitmap = getSnapshotEdges(snapshotId);
        if (!fullBitmap) return 0;
        
        uint64_t count = roaring_bitmap_get_cardinality(fullBitmap);
        roaring_bitmap_free(fullBitmap);
        
        return count;
    }
    
    /**
     * Get memory usage estimate in bytes
     */
    size_t getMemoryUsage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        size_t total = 0;
        
        // Edge catalog
        total += edgeList_.size() * 80;  // Approx 80 bytes per edge (2 strings + overhead)
        total += edgeMap_.size() * 64;   // Map overhead
        
        // Snapshot bitmaps
        for (const auto& [id, snapshot] : snapshots_) {
            if (snapshot->fullEdgeBitmap) {
                total += roaring_bitmap_portable_size_in_bytes(snapshot->fullEdgeBitmap);
            }
            total += roaring_bitmap_portable_size_in_bytes(snapshot->addedEdges);
            total += roaring_bitmap_portable_size_in_bytes(snapshot->removedEdges);
            
            if (snapshot->materializedCache) {
                total += roaring_bitmap_portable_size_in_bytes(snapshot->materializedCache);
            }
        }
        
        return total;
    }
    
    /**
     * Print statistics
     */
    void printStatistics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        printf("\n=== Temporal Store Statistics ===\n");
        printf("Graph ID: %u, Partition ID: %u\n", graphId_, partitionId_);
        printf("Total Snapshots: %zu\n", snapshots_.size());
        printf("Current Snapshot: %u\n", currentSnapshotId_);
        printf("Total Edges Tracked: %zu\n", edgeList_.size());
        printf("Total Edges Added: %lu\n", totalEdgesAdded_);
        printf("Memory Usage: %.2f MB\n", getMemoryUsage() / 1024.0 / 1024.0);
        
        int baseCount = 0, deltaCount = 0, cachedCount = 0;
        for (const auto& [id, snapshot] : snapshots_) {
            if (snapshot->isBase) baseCount++;
            else deltaCount++;
            if (snapshot->isCached) cachedCount++;
        }
        printf("Base Snapshots: %d, Delta Snapshots: %d, Cached: %d\n", 
               baseCount, deltaCount, cachedCount);
        printf("=================================\n\n");
    }
};

#endif // DELTA_ENCODED_TEMPORAL_STORE_H
