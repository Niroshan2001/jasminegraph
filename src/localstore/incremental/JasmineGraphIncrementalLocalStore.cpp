/**
Copyright 2021 JasminGraph Team
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

#include "JasmineGraphIncrementalLocalStore.h"

#include <faiss/IndexFlat.h>

#include <memory>
#include <stdexcept>
#include <mutex>
#include <vector>
#include <atomic>
#include <limits>
#include <sstream>
#include <fstream>
#include <iomanip>
#if defined(ENABLE_PROMETHEUS_CPP)
  /* Only include prometheus-cpp headers if they are actually available on the
     include path. Some build environments may define ENABLE_PROMETHEUS_CPP via
     CMake but not have headers installed (e.g., partial package). Use
     __has_include to protect against missing headers. */
#  if defined(__has_include) && __has_include(<prometheus/push.h>)
#    define USE_PROMETHEUS_CPP 1
#    include <prometheus/registry.h>
#    include <prometheus/histogram.h>
#    include <prometheus/push.h>
#    include <prometheus/collectable.h>
#  endif
#endif

#include "../../nativestore/MetaPropertyLink.h"
#include "../../nativestore/RelationBlock.h"
#include "../../util/Utils.h"
#include "../../util/logger/Logger.h"
#include "../../vectorstore/FaissIndex.h"
#include "../../vectorstore/TextEmbedder.h"

Logger incremental_localstore_logger;

// Simple in-process histogram implementation to aggregate latency observations
// and push cumulative bucket counts to Pushgateway so Prometheus can compute
// percentiles via histogram_quantile(). This avoids adding a C++ Prometheus
// client dependency while producing correct bucket counts.
struct IngestLatencyHistogram {
  std::vector<double> le;            // bucket upper bounds (ms)
  std::vector<uint64_t> counts;     // per-bucket (non-cumulative) counts
  uint64_t count = 0;               // total observations
  double sum = 0.0;                 // sum of latencies
  std::mutex mtx;
  std::atomic<uint64_t> observations_since_push{0};
  IngestLatencyHistogram() {
    // Example buckets in milliseconds (suitable for edge ingest latencies)
    le = {5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
    counts.assign(le.size() + 1, 0); // extra slot for +Inf
  }
} ingestHistogram;

static size_t HIST_PUSH_THRESHOLD = 1; // push snapshot after this many observations

static void pushHistogramSnapshot() {
  std::lock_guard<std::mutex> guard(ingestHistogram.mtx);
  // Build cumulative counts
  std::vector<uint64_t> cumulative(ingestHistogram.counts.size());
  uint64_t running = 0;
  for (size_t i = 0; i < ingestHistogram.counts.size(); ++i) {
    running += ingestHistogram.counts[i];
    cumulative[i] = running;
  }

  // Push each bucket as a metric with le label. Use Utils::send_job to push
  // lines like: jasminegraph_ingestion_latency_ms_bucket{le="10"} 123
  for (size_t i = 0; i < cumulative.size(); ++i) {
    std::ostringstream metricName;
    if (i < ingestHistogram.le.size()) {
      // finite upper bound
      metricName << "jasminegraph_ingestion_latency_ms_bucket{le=\"" << std::fixed << std::noshowpoint << ingestHistogram.le[i] << "\"}";
    } else {
      metricName << "jasminegraph_ingestion_latency_ms_bucket{le=\"+Inf\"}";
    }
    Utils::send_job("", metricName.str(), std::to_string(cumulative[i]));
  }

  // Push count and sum
  Utils::send_job("", "jasminegraph_ingestion_latency_ms_count", std::to_string(ingestHistogram.count));
  // sum as double; format without scientific notation
  std::ostringstream sumStr;
  sumStr << std::fixed << ingestHistogram.sum;
  Utils::send_job("", "jasminegraph_ingestion_latency_ms_sum", sumStr.str());

  ingestHistogram.observations_since_push.store(0);
}

#ifdef USE_PROMETHEUS_CPP
// prometheus-cpp backed histogram + pushgateway helper
static std::shared_ptr<prometheus::Registry> promRegistry;
static prometheus::Histogram* promLatencyHist = nullptr;
static std::unique_ptr<prometheus::PushGateway> promPushGateway;
static std::mutex promInitMutex;

static void initPrometheusClient() {
  std::lock_guard<std::mutex> guard(promInitMutex);
  if (promLatencyHist != nullptr) return;
  promRegistry = std::make_shared<prometheus::Registry>();
  std::vector<double> buckets = {5,10,25,50,100,250,500,1000,2500,5000,10000};
  auto& family = prometheus::BuildHistogram()
                     .Name("jasminegraph_ingestion_latency_ms")
                     .Help("Per-edge ingestion->persistence latency (ms)")
                     .Register(*promRegistry);
  promLatencyHist = &family.Add({}, prometheus::Histogram::BucketBoundaries{buckets});

  // Prepare PushGateway client
  std::string pushGatewayAddr;
  if (jasminegraph_profile == PROFILE_K8S) {
    std::unique_ptr<K8sInterface> interface(new K8sInterface());
    pushGatewayAddr = interface->getJasmineGraphConfig("pushgateway_address");
  } else {
    pushGatewayAddr = Utils::getJasmineGraphProperty("org.jasminegraph.collector.pushgateway");
  }
  if (!pushGatewayAddr.empty()) {
    promPushGateway = std::make_unique<prometheus::PushGateway>(pushGatewayAddr);
  }
}
#endif

JasmineGraphIncrementalLocalStore::JasmineGraphIncrementalLocalStore(
    unsigned int graphID, unsigned int partitionID, std::string openMode,
    bool embedNode) {
  gc.graphID = graphID;
  gc.partitionID = partitionID;
  gc.maxLabelSize = std::stoi(Utils::getJasmineGraphProperty(
      "org.jasminegraph.nativestore.max.label.size"));
  this->embedNode = embedNode;
  this->embedding_requests = new std::vector<EmbeddingRequest>();

  gc.openMode = openMode;
  this->nm = new NodeManager(gc);
  if (this->embedNode) {
    incremental_localstore_logger.info("Embedding enabled for the local store");
    this->faissStore =
        FaissIndex::getInstance(std::stoi(Utils::getJasmineGraphProperty(
                                    "org.jasminegraph.vectorstore.dimension")),
                                this->nm->getDbPrefix() + "_faiss.index");
    this->textEmbedder = new TextEmbedder(
        Utils::getJasmineGraphProperty("org.jasminegraph.vectorstore.embedding."
                                       "ollama.endpoint"),  // Ollama endpoint
        Utils::getJasmineGraphProperty(
            "org.jasminegraph.vectorstore.embedding.model"));
  }
};

JasmineGraphIncrementalLocalStore::~JasmineGraphIncrementalLocalStore() {
  printAndSaveHistogram();
  if (nm) {
    nm->close();
    delete nm;
    nm = nullptr;
  }

  if (embedding_requests) {
    delete embedding_requests;
    embedding_requests = nullptr;
  }

  if (embedNode && textEmbedder) {
    delete textEmbedder;
    textEmbedder = nullptr;
  }
}

bool JasmineGraphIncrementalLocalStore::getAndStoreEmbeddings() {
  std::vector<string> batch_request;
  for (EmbeddingRequest& request : *embedding_requests) {
    batch_request.emplace_back(request.nodeText);
  }
  vector<vector<float>> results = textEmbedder->batch_embed(batch_request);

  for (size_t i = 0; i < results.size(); ++i) {
    faissStore->add(results[i], embedding_requests->at(i).nodeId);
  }
  embedding_requests->clear();
  faissStore->save();
  return true;
}

void JasmineGraphIncrementalLocalStore::printAndSaveHistogram() {
  std::lock_guard<std::mutex> guard(ingestHistogram.mtx);
  if (ingestHistogram.count == 0) {
    return;
  }

  std::string folderLocation = Utils::getJasmineGraphProperty("org.jasminegraph.server.instance.datafolder");
  if (folderLocation.empty()) {
    folderLocation = "/var/tmp/jasminegraph-localstore-tmp";
  }
  Utils::createDirectory(folderLocation);

  std::string filePath = folderLocation + "/latency_histogram_graph_" + 
                         std::to_string(gc.graphID) + "_partition_" + 
                         std::to_string(gc.partitionID) + ".txt";

  std::ofstream outFile(filePath, std::ios::out | std::ios::trunc);
  if (!outFile.is_open()) {
    incremental_localstore_logger.error("Failed to open latency histogram file: " + filePath);
    return;
  }

  // Build cumulative counts
  std::vector<uint64_t> cumulative(ingestHistogram.counts.size());
  uint64_t running = 0;
  for (size_t i = 0; i < ingestHistogram.counts.size(); ++i) {
    running += ingestHistogram.counts[i];
    cumulative[i] = running;
  }

  std::ostringstream oss;
  oss << "\n==================================================\n";
  oss << " LATENCY HISTOGRAM (Graph: " << gc.graphID << ", Partition: " << gc.partitionID << ")\n";
  oss << "==================================================\n";
  oss << "Total edges persisted: " << ingestHistogram.count << "\n";
  oss << "Sum latency: " << std::fixed << std::setprecision(2) << ingestHistogram.sum << " ms\n";
  if (ingestHistogram.count > 0) {
    oss << "Average latency: " << std::fixed << std::setprecision(2) << (ingestHistogram.sum / ingestHistogram.count) << " ms\n";
  } else {
    oss << "Average latency: 0.0 ms\n";
  }
  oss << "--------------------------------------------------\n";
  oss << "Buckets:\n";

  for (size_t i = 0; i < cumulative.size(); ++i) {
    if (i < ingestHistogram.le.size()) {
      oss << "  le <= " << std::setw(6) << std::fixed << std::noshowpoint << ingestHistogram.le[i] 
          << " ms: " << cumulative[i] << " (" 
          << std::fixed << std::setprecision(2) << (ingestHistogram.count > 0 ? (cumulative[i] * 100.0 / ingestHistogram.count) : 0.0) << "%)\n";
    } else {
      oss << "  le <=    +Inf ms: " << cumulative[i] << " (100.0%)\n";
    }
  }
  oss << "==================================================\n";

  std::string outputStr = oss.str();
  incremental_localstore_logger.info(outputStr);
  outFile << outputStr;
  outFile.close();
}


std::pair<std::string, unsigned int> JasmineGraphIncrementalLocalStore::getIDs(
    std::string edgeString) {
  try {
    auto edgeJson = json::parse(edgeString);
    if (edgeJson.contains("properties")) {
      auto edgeProperties = edgeJson["properties"];
      return {edgeProperties["graphId"], edgeJson["PID"]};
    }
  } catch (const std::exception&
               e) {  // TODO tmkasun: Handle multiple types of exceptions
    incremental_localstore_logger.log(
        "Error while processing edge data = " + std::string(e.what()) +
            "Could be due to JSON parsing error or error while persisting the "
            "data to disk",
        "error");
  }
  return {"", 0};  // all plath of the function must return
                   // std::pair<std::string, unsigned int>
                   // type object even there is an error
}

void JasmineGraphIncrementalLocalStore::addEdgeFromString(
    std::string edgeString) {
  try {
    addEdgeFromJson(json::parse(edgeString));
  } catch (const json::parse_error &ex) {
    incremental_localstore_logger.error(
        "JSON parse error while processing edge string: " + edgeString +
        " | Error: " + std::string(ex.what()));
  } catch (const std::exception &ex) {
    incremental_localstore_logger.error(
        "Unhandled exception while processing edge data: " + edgeString +
        " | Error: " + std::string(ex.what()));
  } catch (...) {
    incremental_localstore_logger.error(
        "Unknown fatal error while processing: " + edgeString);
  }
}

void JasmineGraphIncrementalLocalStore::addEdgeFromJson(const json& edgeJson) {
  try {
    if (edgeJson.contains("isNode")) {
      std::string nodeId = edgeJson["id"];
      NodeBlock* newNode = this->nm->addNode(nodeId);

      char value[PropertyLink::MAX_VALUE_SIZE] = {};
      char meta[MetaPropertyLink::MAX_VALUE_SIZE] = {};

      if (edgeJson.contains("properties")) {
        auto sourceProps = edgeJson["properties"];
        for (auto it = sourceProps.begin(); it != sourceProps.end(); it++) {
          strcpy(value, it.value().get<std::string>().c_str());
          newNode->addProperty(std::string(it.key()), &value[0]);
        }
      }

      std::string sourcePid = std::to_string(edgeJson["pid"].get<int>());
      strcpy(meta, sourcePid.c_str());
      newNode->addMetaProperty(MetaPropertyLink::PARTITION_ID, &meta[0]);
      delete newNode;
      return;
    }

    auto sourceJson = edgeJson["source"];
    auto destinationJson = edgeJson["destination"];

    std::string sId = std::string(sourceJson["id"]);
    std::string dId = std::string(destinationJson["id"]);

    bool isLocal = (edgeJson["EdgeType"] == "Local");

    RelationBlock* newRelation;
    if (isLocal) {
      newRelation = this->nm->addLocalEdge({sId, dId});
    } else {
      newRelation = this->nm->addCentralEdge({sId, dId});
    }
    if (!newRelation) {
      return;
    }

    if (isLocal) {
      addLocalEdgeProperties(newRelation, edgeJson);
    } else {
      addCentralEdgeProperties(newRelation, edgeJson);
    }

    addSourceProperties(newRelation, sourceJson);
    addDestinationProperties(newRelation, destinationJson);

    // Release per-edge objects created by NodeManager for streaming ingest.
    delete newRelation->getSource();
    delete newRelation->getDestination();
    delete newRelation;

    incremental_localstore_logger.debug("Edge (" + sId + ", " + dId +
                                        ") Added successfully!");
    // If the edge JSON included ingest metadata, compute latency and push metric
    try {
      if (edgeJson.contains("properties") && edgeJson["properties"].contains("__ingest_ts_ms")) {
        long long ingest_ts = 0;
        try {
          ingest_ts = edgeJson["properties"]["__ingest_ts_ms"].get<long long>();
        } catch (...) {
          // fallback if stored as string
          try {
            ingest_ts = std::stoll(edgeJson["properties"]["__ingest_ts_ms"].get<std::string>());
          } catch (...) { ingest_ts = 0; }
        }
        if (ingest_ts > 0) {
          long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          long long latency_ms = now_ms - ingest_ts;

          // Update in-process histogram
          {
            std::lock_guard<std::mutex> guard(ingestHistogram.mtx);
            // find the first bucket with le >= latency_ms
            bool placed = false;
            for (size_t i = 0; i < ingestHistogram.le.size(); ++i) {
              if (latency_ms <= static_cast<long long>(ingestHistogram.le[i])) {
                ingestHistogram.counts[i]++;
                placed = true;
                break;
              }
            }
            if (!placed) {
              // +Inf bucket
              ingestHistogram.counts.back()++;
            }
            ingestHistogram.count++;
            ingestHistogram.sum += static_cast<double>(latency_ms);
            ingestHistogram.observations_since_push.fetch_add(1);

            // Progress logging: print current histogram state every 100,000 observations
            if (ingestHistogram.count % 100000 == 0) {
              std::vector<uint64_t> cumulative(ingestHistogram.counts.size());
              uint64_t running = 0;
              for (size_t j = 0; j < ingestHistogram.counts.size(); ++j) {
                running += ingestHistogram.counts[j];
                cumulative[j] = running;
              }
              std::ostringstream progressOss;
              progressOss << "\n--- Latency Histogram Progress (Count: " << ingestHistogram.count << ") ---\n";
              progressOss << "Average latency: " << std::fixed << std::setprecision(2) << (ingestHistogram.sum / ingestHistogram.count) << " ms\n";
              for (size_t j = 0; j < cumulative.size(); ++j) {
                if (j < ingestHistogram.le.size()) {
                  progressOss << "  le <= " << std::fixed << std::noshowpoint << ingestHistogram.le[j] << " ms: " << cumulative[j] << "\n";
                } else {
                  progressOss << "  le <=    +Inf ms: " << cumulative[j] << "\n";
                }
              }
              progressOss << "--------------------------------------------------------\n";
              incremental_localstore_logger.info(progressOss.str());
            }
          }

          bool pushed_via_prom = false;
#ifdef USE_PROMETHEUS_CPP
          initPrometheusClient();
          if (promLatencyHist) {
            promLatencyHist->Observe(static_cast<double>(latency_ms));
            // Push periodically to Pushgateway to avoid high overhead
            if (!promPushGateway) {
              // fallback: do nothing, rely on registry being scraped (not configured)
            } else {
              // push with job name 'jasminegraph' and no grouping labels
              try {
                promPushGateway->PushAdd(*promRegistry, "jasminegraph");
                pushed_via_prom = true;
              } catch (...) {
                incremental_localstore_logger.debug("prometheus push failed");
              }
            }
          }
#endif
          if (!pushed_via_prom) {
            // Periodically push snapshot so Prometheus sees cumulative buckets
            if (ingestHistogram.observations_since_push.load() >= HIST_PUSH_THRESHOLD) {
              pushHistogramSnapshot();
            }
          }
        }
      }
    } catch (...) {
      incremental_localstore_logger.debug("Failed to compute/push ingestion latency metric");
    }
  } catch (const json::type_error &ex) {
    incremental_localstore_logger.error(
        "JSON type error in addEdgeFromJson: " + std::string(ex.what()));
  } catch (const json::out_of_range &ex) {
    incremental_localstore_logger.error(
        "JSON out-of-range in addEdgeFromJson: " + std::string(ex.what()));
  } catch (const std::exception &ex) {
    incremental_localstore_logger.error(
        "Exception in addEdgeFromJson: " + std::string(ex.what()));
  } catch (...) {
    incremental_localstore_logger.error("Unknown error in addEdgeFromJson");
  }
}

void JasmineGraphIncrementalLocalStore::addLocalEdge(std::string edge) {
  auto jsonEdge = json::parse(edge);
  auto jsonSource = jsonEdge["source"];
  auto jsonDestination = jsonEdge["destination"];

  // log the edge information
  if (!jsonSource.contains("id") || !jsonDestination.contains("id")) {
    incremental_localstore_logger.error(
        "Source or destination ID missing in edge data: " + edge);
    return;
  }
  if (!jsonEdge.contains("source") || !jsonEdge.contains("destination")) {
    incremental_localstore_logger.error(
        "Source or destination missing in edge data: " + edge);
    return;
  }
  if (!jsonEdge.contains("properties")) {
    incremental_localstore_logger.error("Properties missing in edge data: " +
                                        edge);
    return;
  }
  if (!jsonSource.contains("pid") || !jsonDestination.contains("pid")) {
    incremental_localstore_logger.error(
        "Partition ID missing in source or destination: " + edge);
    return;
  }

  std::string sId = std::string(jsonSource["id"]);
  std::string dId = std::string(jsonDestination["id"]);
  RelationBlock* newRelation = nullptr;
  if (jsonEdge["properties"].contains("id")) {
    std::string edgeId = std::string(jsonEdge["properties"]["id"]);

    if (this->nm->edgeIndex.find(edgeId) == this->nm->edgeIndex.end()) {
      incremental_localstore_logger.debug("Edge Id not found: " + edgeId);

      newRelation = this->nm->addLocalEdge({sId, dId});
      this->nm->edgeIndex.insert({edgeId, this->nm->nextEdgeIndex});
    } else {
      incremental_localstore_logger.debug("Edge Id already found: " + edgeId);
    }
  } else {
    newRelation = this->nm->addLocalEdge({sId, dId});
  }

  if (newRelation == nullptr) {
    return;
  }

  addLocalEdgeProperties(newRelation, jsonEdge);
  addSourceProperties(newRelation, jsonSource);
  addDestinationProperties(newRelation, jsonDestination);
  delete newRelation->getSource();
  delete newRelation->getDestination();
  delete newRelation;
  incremental_localstore_logger.debug("Local edge (" + sId + "-> " + dId +
                                      " ) added successfully");
}

void JasmineGraphIncrementalLocalStore::addCentralEdge(std::string edge) {
  auto jsonEdge = json::parse(edge);
  auto jsonSource = jsonEdge["source"];
  auto jsonDestination = jsonEdge["destination"];

  std::string sId = std::string(jsonSource["id"]);
  std::string dId = std::string(jsonDestination["id"]);

  RelationBlock* newRelation = nullptr;

  if (jsonEdge["properties"].contains("id")) {
    std::string edgeId = std::string(jsonEdge["properties"]["id"]);

    if (this->nm->edgeIndex.find(edgeId) == this->nm->edgeIndex.end()) {
      incremental_localstore_logger.debug("Edge Id not found: " + edgeId);

      newRelation = this->nm->addCentralEdge({sId, dId});
      this->nm->edgeIndex.insert({edgeId, this->nm->nextEdgeIndex});
    } else {
      incremental_localstore_logger.debug("Edge Id already found: " + edgeId);
    }
  } else {
    newRelation = this->nm->addCentralEdge({sId, dId});
  }

  if (newRelation == nullptr) {
    return;
  }

  addCentralEdgeProperties(newRelation, jsonEdge);
  addSourceProperties(newRelation, jsonSource);
  addDestinationProperties(newRelation, jsonDestination);
  delete newRelation->getSource();
  delete newRelation->getDestination();
  delete newRelation;
  incremental_localstore_logger.debug("Central edge (" + sId + "-> " + dId +
                                      " ) added successfully");
}

void JasmineGraphIncrementalLocalStore::addCentralEdgeProperties(
    RelationBlock* relationBlock, const json& edgeJson) {
  char value[PropertyLink::MAX_VALUE_SIZE] = {};
  char type[RelationBlock::MAX_TYPE_SIZE] = {0};
  if (edgeJson.contains("properties")) {
    auto edgeProperties = edgeJson["properties"];
    for (auto it = edgeProperties.begin(); it != edgeProperties.end(); it++) {
      strcpy(value, it.value().get<std::string>().c_str());
      if (std::string(it.key()) == "type") {
        strcpy(type, it.value().get<std::string>().c_str());
        relationBlock->addCentralRelationshipType(&type[0]);
      }
      relationBlock->addCentralProperty(std::string(it.key()), &value[0]);
    }
  }
  std::string edgePid = std::to_string(edgeJson["source"]["pid"].get<int>());
  addRelationMetaProperty(relationBlock, MetaPropertyEdgeLink::PARTITION_ID,
                          edgePid);
}

void JasmineGraphIncrementalLocalStore::addLocalEdgeProperties(
    RelationBlock* relationBlock, const json& edgeJson) {
  char value[PropertyLink::MAX_VALUE_SIZE] = {};
  char type[RelationBlock::MAX_TYPE_SIZE] = {0};
  if (edgeJson.contains("properties")) {
    auto edgeProperties = edgeJson["properties"];
    for (auto it = edgeProperties.begin(); it != edgeProperties.end(); it++) {
      strcpy(value, it.value().get<std::string>().c_str());
      if (std::string(it.key()) == "type") {
        strcpy(type, it.value().get<std::string>().c_str());
        relationBlock->addLocalRelationshipType(&type[0]);
      }
      relationBlock->addLocalProperty(std::string(it.key()), &value[0]);
    }
  }
}

void JasmineGraphIncrementalLocalStore::addSourceProperties(
    RelationBlock* relationBlock, const json& sourceJson) {
  char value[PropertyLink::MAX_VALUE_SIZE] = {};
  char label[NodeBlock::LABEL_SIZE] = {0};
  std::ostringstream textForEmbedding;

  if (sourceJson.contains("properties")) {
    auto sourceProps = json(sourceJson["properties"]);

    if (!sourceProps.empty()) {
      for (auto it = sourceProps.begin(); it != sourceProps.end(); it++) {
        strcpy(value, it.value().get<std::string>().c_str());
        if (std::string(it.key()) == "label") {
          strcpy(label, it.value().get<std::string>().c_str());
          relationBlock->getSource()->addLabel(&label[0]);
        }
        textForEmbedding << it.key() << ":" << value << " ";
        relationBlock->getSource()->addProperty(std::string(it.key()),
                                                &value[0]);
      }

      if (this->embedNode) {
        std::string nodeText = textForEmbedding.str();
        if (!nodeText.empty()) {
          if (faissStore->getEmbeddingById(sourceJson["id"]).size() == 0) {
            incremental_localstore_logger.error(
                "Node with ID " + sourceJson["id"].get<std::string>() +
                " found . Skipping ");
            return;
          }
          EmbeddingRequest request = {sourceJson["id"].get<std::string>(),
                                      nodeText};
          embedding_requests->emplace_back(request);
        }
      }
    }
  }

  std::string sourcePid = std::to_string(sourceJson["pid"].get<int>());
  addNodeMetaProperty(relationBlock->getSource(),
                      MetaPropertyLink::PARTITION_ID, sourcePid);
}

void JasmineGraphIncrementalLocalStore::addDestinationProperties(
    RelationBlock* relationBlock, const json& destinationJson) {
  char value[PropertyLink::MAX_VALUE_SIZE] = {};
  char label[NodeBlock::LABEL_SIZE] = {0};
  std::ostringstream textForEmbedding;

  if (destinationJson.contains("properties")) {
    auto destinationProps = destinationJson["properties"];
    if (!destinationProps.empty()) {
      for (auto it = destinationProps.begin(); it != destinationProps.end();
           it++) {
        strcpy(value, it.value().get<std::string>().c_str());
        if (std::string(it.key()) == "label") {
          strcpy(label, it.value().get<std::string>().c_str());
          relationBlock->getDestination()->addLabel(&label[0]);
        }
        textForEmbedding << it.key() << ":" << value << " ";

        relationBlock->getDestination()->addProperty(std::string(it.key()),
                                                     &value[0]);
      }
      if (this->embedNode) {
        std::string nodeText = textForEmbedding.str();
        if (!nodeText.empty()) {
          if (faissStore->getEmbeddingById(destinationJson["id"]).empty()) {
            incremental_localstore_logger.error(
                "Node with ID " + destinationJson["id"].get<std::string>() +
                " found . Skipping ");
            return;
          }
          EmbeddingRequest request = {destinationJson["id"].get<std::string>(),
                                      nodeText};
          embedding_requests->emplace_back(request);
        }
      }
    }
  }
  std::string destPId = std::to_string(destinationJson["pid"].get<int>());
  addNodeMetaProperty(relationBlock->getDestination(),
                      MetaPropertyLink::PARTITION_ID, destPId);
}

void JasmineGraphIncrementalLocalStore::addNodeMetaProperty(
    NodeBlock* nodeBlock, std::string propertyKey, std::string propertyValue) {
  incremental_localstore_logger.debug("meta property: " + propertyKey + " " +
                                      propertyValue);
  char meta[MetaPropertyLink::MAX_VALUE_SIZE] = {};
  strcpy(meta, propertyValue.c_str());
  nodeBlock->addMetaProperty(propertyKey, &meta[0]);
}

void JasmineGraphIncrementalLocalStore::addRelationMetaProperty(
    RelationBlock* relationBlock, std::string propertyKey,
    std::string propertyValue) {
  char meta[MetaPropertyEdgeLink::MAX_VALUE_SIZE] = {};
  strcpy(meta, propertyValue.c_str());
  relationBlock->addMetaProperty(propertyKey, &meta[0]);
}
