/**
Copyright 2025 JasmineGraph Team
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

#pragma once

/**
 * IngestionMetrics — lightweight, zero-extra-dependency histogram tracker.
 *
 * Accumulates per-edge latency (T0 arrival → T1 persisted) and related
 * timing data as Prometheus histogram buckets, then POSTs the result to
 * a Prometheus Pushgateway via libcurl (already a project dependency).
 *
 * Workers call this from their consumer threads; the final push() happens
 * once at the end of WorkerKafkaConsumer::run(), so there is no
 * network I/O on the hot path.
 *
 * Thread-safety: all record*() methods use std::atomic with relaxed
 * ordering — correct for independent counters that are only read once
 * at the end (in push()).
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "../logger/Logger.h"

class IngestionMetrics {
 public:
    // ── Latency histogram buckets (microseconds) ─────────────────────────────
    // Covers sub-millisecond fast paths up to 50 ms outliers.
    static constexpr int NUM_LAT_BUCKETS = 13;
    static constexpr uint64_t LAT_BOUNDS[NUM_LAT_BUCKETS] = {
        10, 50, 100, 250, 500,
        1000, 2500, 5000, 10000, 25000,
        50000, 100000,
        UINT64_MAX  // +Inf sentinel — must be last
    };

    // ── Snapshot duration buckets (milliseconds) ─────────────────────────────
    static constexpr int NUM_SNAP_BUCKETS = 10;
    static constexpr uint64_t SNAP_BOUNDS[NUM_SNAP_BUCKETS] = {
        1, 5, 10, 50, 100, 250, 500, 1000, 5000,
        UINT64_MAX
    };

    // ── Constructor ──────────────────────────────────────────────────────────
    IngestionMetrics() {
        for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
            localLatBuckets_[i].store(0, std::memory_order_relaxed);
            centralLatBuckets_[i].store(0, std::memory_order_relaxed);
            persistBuckets_[i].store(0, std::memory_order_relaxed);
        }
        for (int i = 0; i < NUM_SNAP_BUCKETS; ++i) {
            snapBuckets_[i].store(0, std::memory_order_relaxed);
        }
        localLatSum_.store(0, std::memory_order_relaxed);
        centralLatSum_.store(0, std::memory_order_relaxed);
        localLatCount_.store(0, std::memory_order_relaxed);
        centralLatCount_.store(0, std::memory_order_relaxed);
        persistSum_.store(0, std::memory_order_relaxed);
        persistCount_.store(0, std::memory_order_relaxed);
        snapSum_.store(0, std::memory_order_relaxed);
        snapCount_.store(0, std::memory_order_relaxed);
        totalEdges_.store(0, std::memory_order_relaxed);
    }

    // Disable copy — atomics are not copyable
    IngestionMetrics(const IngestionMetrics&) = delete;
    IngestionMetrics& operator=(const IngestionMetrics&) = delete;

    // ── Record edge arrival→persist latency (microseconds) ───────────────────
    // isLocal = true  → both endpoints in the same partition (local edge)
    // isLocal = false → cross-partition (central edge)
    inline void recordEdgeLatency(uint64_t micros, bool isLocal) {
        totalEdges_.fetch_add(1, std::memory_order_relaxed);
        if (isLocal) {
            localLatSum_.fetch_add(micros, std::memory_order_relaxed);
            localLatCount_.fetch_add(1, std::memory_order_relaxed);
            for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
                if (micros <= LAT_BOUNDS[i]) {
                    localLatBuckets_[i].fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            centralLatSum_.fetch_add(micros, std::memory_order_relaxed);
            centralLatCount_.fetch_add(1, std::memory_order_relaxed);
            for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
                if (micros <= LAT_BOUNDS[i]) {
                    centralLatBuckets_[i].fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    // ── Record time spent inside persistence calls (microseconds) ────────────
    inline void recordPersistDuration(uint64_t micros) {
        persistSum_.fetch_add(micros, std::memory_order_relaxed);
        persistCount_.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
            if (micros <= LAT_BOUNDS[i]) {
                persistBuckets_[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // ── Record time to complete a global snapshot (milliseconds) ─────────────
    inline void recordSnapshotDuration(uint64_t ms) {
        snapSum_.fetch_add(ms, std::memory_order_relaxed);
        snapCount_.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < NUM_SNAP_BUCKETS; ++i) {
            if (ms <= SNAP_BOUNDS[i]) {
                snapBuckets_[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // ── Format & push to Prometheus Pushgateway ───────────────────────────────
    // url  e.g. "http://10.8.100.247:9091"
    // job  label for the push job, e.g. "jasminegraph_worker"
    // graphId / workerIndex for metric labels
    bool push(const std::string& url, const std::string& job,
              int graphId, int workerIndex) const {
        if (url.empty()) {
            return false;
        }

        std::string body = buildPrometheusText(graphId, workerIndex);

        // POST to /metrics/job/<job>/instance/<workerIndex>
        std::string endpoint = url + "/metrics/job/" + job +
                               "/instance/worker_" + std::to_string(workerIndex);

        Logger logger;
        logger.info("[METRICS] Pushing " + std::to_string(body.size()) +
                    " bytes to " + endpoint);

        CURL* curl = curl_easy_init();
        if (!curl) {
            logger.error("[METRICS] curl_easy_init() failed");
            return false;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: text/plain; version=0.0.4");

        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        // Suppress output
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         +[](void*, size_t s, size_t n, void*) -> size_t { return s * n; });

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            logger.error("[METRICS] curl error: " + std::string(curl_easy_strerror(res)));
            return false;
        }
        if (httpCode < 200 || httpCode >= 300) {
            logger.error("[METRICS] Pushgateway returned HTTP " + std::to_string(httpCode));
            return false;
        }
        logger.info("[METRICS] Push succeeded (HTTP " + std::to_string(httpCode) + ")");
        return true;
    }

 private:
    // ── Latency histograms ────────────────────────────────────────────────────
    std::atomic<uint64_t> localLatBuckets_[NUM_LAT_BUCKETS];
    std::atomic<uint64_t> centralLatBuckets_[NUM_LAT_BUCKETS];
    std::atomic<uint64_t> localLatSum_;
    std::atomic<uint64_t> localLatCount_;
    std::atomic<uint64_t> centralLatSum_;
    std::atomic<uint64_t> centralLatCount_;

    // ── Persist duration histogram ────────────────────────────────────────────
    std::atomic<uint64_t> persistBuckets_[NUM_LAT_BUCKETS];
    std::atomic<uint64_t> persistSum_;
    std::atomic<uint64_t> persistCount_;

    // ── Snapshot duration histogram ───────────────────────────────────────────
    std::atomic<uint64_t> snapBuckets_[NUM_SNAP_BUCKETS];
    std::atomic<uint64_t> snapSum_;
    std::atomic<uint64_t> snapCount_;

    // ── Simple counter ────────────────────────────────────────────────────────
    std::atomic<uint64_t> totalEdges_;

    // ── Build Prometheus exposition format text ───────────────────────────────
    std::string buildPrometheusText(int graphId, int workerIndex) const {
        std::ostringstream ss;
        const std::string g  = std::to_string(graphId);
        const std::string wi = std::to_string(workerIndex);

        // Helper: write one histogram (latency, us)
        auto writeLatHist = [&](const char* name, const char* help,
                                const std::atomic<uint64_t>* buckets,
                                uint64_t sum, uint64_t count,
                                const char* edgeType) {
            ss << "# HELP " << name << " " << help << "\n";
            ss << "# TYPE " << name << " histogram\n";
            for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
                ss << name << "_bucket{graph_id=\"" << g
                   << "\",worker=\"" << wi
                   << "\",edge_type=\"" << edgeType << "\",le=\"";
                if (LAT_BOUNDS[i] == UINT64_MAX) {
                    ss << "+Inf";
                } else {
                    ss << LAT_BOUNDS[i];
                }
                ss << "\"} " << buckets[i].load(std::memory_order_relaxed) << "\n";
            }
            ss << name << "_sum{graph_id=\"" << g
               << "\",worker=\"" << wi
               << "\",edge_type=\"" << edgeType << "\"} " << sum << "\n";
            ss << name << "_count{graph_id=\"" << g
               << "\",worker=\"" << wi
               << "\",edge_type=\"" << edgeType << "\"} " << count << "\n";
        };

        // ── jg_edge_ingestion_latency_us ─────────────────────────────────────
        writeLatHist(
            "jg_edge_ingestion_latency_us",
            "Edge arrival to persistence latency in microseconds",
            localLatBuckets_,
            localLatSum_.load(std::memory_order_relaxed),
            localLatCount_.load(std::memory_order_relaxed),
            "local");

        writeLatHist(
            "jg_edge_ingestion_latency_us",
            "Edge arrival to persistence latency in microseconds",
            centralLatBuckets_,
            centralLatSum_.load(std::memory_order_relaxed),
            centralLatCount_.load(std::memory_order_relaxed),
            "central");

        // ── jg_edge_persist_duration_us ──────────────────────────────────────
        ss << "# HELP jg_edge_persist_duration_us"
           << " Time spent inside temporal addEdge + native handleRequest (us)\n";
        ss << "# TYPE jg_edge_persist_duration_us histogram\n";
        for (int i = 0; i < NUM_LAT_BUCKETS; ++i) {
            ss << "jg_edge_persist_duration_us_bucket{graph_id=\"" << g
               << "\",worker=\"" << wi << "\",le=\"";
            if (LAT_BOUNDS[i] == UINT64_MAX) ss << "+Inf";
            else ss << LAT_BOUNDS[i];
            ss << "\"} " << persistBuckets_[i].load(std::memory_order_relaxed) << "\n";
        }
        ss << "jg_edge_persist_duration_us_sum{graph_id=\"" << g
           << "\",worker=\"" << wi << "\"} "
           << persistSum_.load(std::memory_order_relaxed) << "\n";
        ss << "jg_edge_persist_duration_us_count{graph_id=\"" << g
           << "\",worker=\"" << wi << "\"} "
           << persistCount_.load(std::memory_order_relaxed) << "\n";

        // ── jg_snapshot_create_duration_ms ───────────────────────────────────
        ss << "# HELP jg_snapshot_create_duration_ms"
           << " Time to complete a global snapshot in milliseconds\n";
        ss << "# TYPE jg_snapshot_create_duration_ms histogram\n";
        for (int i = 0; i < NUM_SNAP_BUCKETS; ++i) {
            ss << "jg_snapshot_create_duration_ms_bucket{graph_id=\"" << g
               << "\",worker=\"" << wi << "\",le=\"";
            if (SNAP_BOUNDS[i] == UINT64_MAX) ss << "+Inf";
            else ss << SNAP_BOUNDS[i];
            ss << "\"} " << snapBuckets_[i].load(std::memory_order_relaxed) << "\n";
        }
        ss << "jg_snapshot_create_duration_ms_sum{graph_id=\"" << g
           << "\",worker=\"" << wi << "\"} "
           << snapSum_.load(std::memory_order_relaxed) << "\n";
        ss << "jg_snapshot_create_duration_ms_count{graph_id=\"" << g
           << "\",worker=\"" << wi << "\"} "
           << snapCount_.load(std::memory_order_relaxed) << "\n";

        // ── jg_edge_ingestion_total (counter for rate()) ──────────────────────
        ss << "# HELP jg_edge_ingestion_total"
           << " Total edges ingested by this worker\n";
        ss << "# TYPE jg_edge_ingestion_total counter\n";
        ss << "jg_edge_ingestion_total{graph_id=\"" << g
           << "\",worker=\"" << wi << "\"} "
           << totalEdges_.load(std::memory_order_relaxed) << "\n";

        return ss.str();
    }
};
