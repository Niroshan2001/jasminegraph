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
 */
#include <fstream>
#include <unistd.h>
#include <regex>
#include <set>
#include <algorithm>

#include "CypherQueryExecutor.h"
#include "antlr4-runtime.h"
#include "../../../../../src/query/processor/cypher/astbuilder/ASTBuilder.h"
#include "../../../../../src/query/processor/cypher/astbuilder/ASTNode.h"
#include "../../../../../src/query/processor/cypher/semanticanalyzer/SemanticAnalyzer.h"
#include "../../../../../src/query/processor/cypher/queryplanner/QueryPlanner.h"
#include "../../../../../src/query/processor/cypher/runtime/AggregationFactory.h"
#include "../../../../../src/query/processor/cypher/runtime/Aggregation.h"
#include "../../../../../src/server/JasmineGraphServer.h"
#include "../../../../../src/temporalstore/TemporalStore.h"
#include "../../../../util/telemetry/OpenTelemetryUtil.h"

#include "/home/ubuntu/software/antlr/CypherLexer.h"
#include "/home/ubuntu/software/antlr/CypherParser.h"

Logger cypher_logger;

namespace {

enum class TemporalCountTarget {
    EDGE,
    SOURCE,
    DESTINATION,
    UNKNOWN
};

struct TemporalQueryPlan {
    std::string sourceAlias = "source";
    std::string destinationAlias = "destination";
    bool isCountQuery = false;
    bool countDistinct = false;
    TemporalCountTarget countTarget = TemporalCountTarget::EDGE;
};

std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void parseTemporalMatchAliases(const std::string& queryString,
                               std::string& sourceAlias,
                               std::string& destinationAlias) {
    const std::regex directedPattern(
        R"(\bMATCH\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*-\s*(?:\[[^\]]*\]\s*)?-\s*>\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))",
        std::regex_constants::icase);
    std::smatch directedMatch;
    if (std::regex_search(queryString, directedMatch, directedPattern)) {
        sourceAlias = directedMatch[1].str();
        destinationAlias = directedMatch[2].str();
        return;
    }

    const std::regex undirectedPattern(
        R"(\bMATCH\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*-\s*(?:\[[^\]]*\]\s*)?-\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\))",
        std::regex_constants::icase);
    std::smatch undirectedMatch;
    if (std::regex_search(queryString, undirectedMatch, undirectedPattern)) {
        sourceAlias = undirectedMatch[1].str();
        destinationAlias = undirectedMatch[2].str();
    }
}

bool buildTemporalQueryPlan(const std::string& queryString,
                            TemporalQueryPlan& plan,
                            std::string& errorMessage) {
    const std::regex matchRegex(R"(\bMATCH\b)", std::regex_constants::icase);
    if (!std::regex_search(queryString, matchRegex)) {
        errorMessage = "tmpcp currently supports MATCH queries and optional RETURN count(...)";
        return false;
    }

    parseTemporalMatchAliases(queryString, plan.sourceAlias, plan.destinationAlias);

    const std::regex countRegex(
        R"(\bRETURN\s+count\s*\(\s*(distinct\s+)?([A-Za-z_][A-Za-z0-9_]*|\*)\s*\))",
        std::regex_constants::icase);
    std::smatch countMatch;
    if (!std::regex_search(queryString, countMatch, countRegex)) {
        return true;
    }

    plan.isCountQuery = true;
    plan.countDistinct = !countMatch[1].str().empty();

    std::string target = toLowerCopy(countMatch[2].str());
    if (target == "*") {
        plan.countTarget = TemporalCountTarget::EDGE;
        return true;
    }

    std::string sourceAliasLower = toLowerCopy(plan.sourceAlias);
    std::string destinationAliasLower = toLowerCopy(plan.destinationAlias);

    if (target == sourceAliasLower) {
        plan.countTarget = TemporalCountTarget::SOURCE;
        return true;
    }
    if (target == destinationAliasLower) {
        plan.countTarget = TemporalCountTarget::DESTINATION;
        return true;
    }

    if (plan.countDistinct) {
        errorMessage = "Unsupported tmpcp DISTINCT count target: " + countMatch[2].str() +
                       ". Use DISTINCT on MATCH node aliases only.";
        return false;
    }

    // COUNT(alias) without DISTINCT still maps to row cardinality.
    plan.countTarget = TemporalCountTarget::UNKNOWN;
    return true;
}

uint64_t evaluateTemporalCount(const TemporalQueryPlan& plan,
                               const std::vector<std::pair<std::string, std::string>>& edges) {
    if (!plan.isCountQuery) {
        return 0;
    }

    if (!plan.countDistinct) {
        return static_cast<uint64_t>(edges.size());
    }

    if (plan.countTarget == TemporalCountTarget::EDGE || plan.countTarget == TemporalCountTarget::UNKNOWN) {
        return static_cast<uint64_t>(edges.size());
    }

    std::set<std::string> distinctNodes;
    if (plan.countTarget == TemporalCountTarget::SOURCE) {
        for (const auto& edge : edges) {
            distinctNodes.insert(edge.first);
        }
    } else if (plan.countTarget == TemporalCountTarget::DESTINATION) {
        for (const auto& edge : edges) {
            distinctNodes.insert(edge.second);
        }
    }
    return static_cast<uint64_t>(distinctNodes.size());
}

bool writeLineToSocket(int connFd, const std::string& line) {
    ssize_t result = write(connFd, line.c_str(), line.length());
    if (result < 0) {
        return false;
    }
    result = write(connFd, Conts::CARRIAGE_RETURN_NEW_LINE.c_str(),
                   Conts::CARRIAGE_RETURN_NEW_LINE.size());
    return result >= 0;
}

bool writeTemporalQueryRows(int connFd,
                            const TemporalQueryPlan& plan,
                            const std::vector<std::pair<std::string, std::string>>& edges,
                            const std::string& snapshotFieldName,
                            const json& snapshotFieldValue,
                            std::string& errorMessage) {
    if (plan.isCountQuery) {
        json row;
        row["count"] = evaluateTemporalCount(plan, edges);
        row[snapshotFieldName] = snapshotFieldValue;
        if (!writeLineToSocket(connFd, row.dump())) {
            errorMessage = "Error writing temporal count result to socket";
            return false;
        }
        return true;
    }

    for (const auto& edge : edges) {
        json row;
        row["source"] = edge.first;
        row["destination"] = edge.second;
        row[plan.sourceAlias] = edge.first;
        row[plan.destinationAlias] = edge.second;
        row[snapshotFieldName] = snapshotFieldValue;
        if (!writeLineToSocket(connFd, row.dump())) {
            errorMessage = "Error writing temporal match rows to socket";
            return false;
        }
    }

    return true;
}


std::string getTemporalSnapshotDirForCypher() {
    std::string configuredPath =
        Utils::getJasmineGraphProperty("org.jasminegraph.server.instance.temporalsnapshotfolder");
    if (!configuredPath.empty()) {
        return configuredPath;
    }

    return Utils::getJasmineGraphProperty("org.jasminegraph.server.instance.datafolder") +
           "/temporal_snapshots";
}

bool tryParseTemporalSnapshotClause(const std::string& queryString, uint32_t& snapshotId) {
    const std::regex snapshotRegex(R"(\bAT\s+SNAPSHOT\s+(\d+)\b)",
                                   std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(queryString, match, snapshotRegex)) {
        return false;
    }

    try {
        snapshotId = static_cast<uint32_t>(std::stoul(match[1].str()));
        return true;
    } catch (...) {
        return false;
    }
}

bool tryParseTemporalRangeClause(const std::string& queryString,
                                 uint32_t& startSnapshot,
                                 uint32_t& endSnapshot) {
    const std::regex rangeRegex(R"(\bFROM\s+(\d+)\s+TO\s+(\d+)\b)",
                                std::regex_constants::icase);
    std::smatch match;
    if (!std::regex_search(queryString, match, rangeRegex)) {
        return false;
    }

    try {
        startSnapshot = static_cast<uint32_t>(std::stoul(match[1].str()));
        endSnapshot = static_cast<uint32_t>(std::stoul(match[2].str()));
        return startSnapshot <= endSnapshot;
    } catch (...) {
        return false;
    }
}

bool loadBitmapFilesForGraphAtSnapshot(int graphId,
                                       uint32_t snapshotId,
                                       std::vector<std::pair<std::string, std::string>>& edges,
                                       size_t& partitionsLoaded,
                                       std::string& errorMessage) {
    std::string snapshotDir = getTemporalSnapshotDirForCypher();
    std::vector<std::string> files = Utils::getListOfFilesInDirectory(snapshotDir);
    std::regex filePattern("^graph" + std::to_string(graphId) +
                           "_part([0-9]+)_bitmaps\\\\.ebm$");

    std::set<std::pair<std::string, std::string>> uniqueEdges;
    partitionsLoaded = 0;

    uint64_t timeThreshold = 60;
    uint64_t edgeThreshold = 10000;

    for (const auto& file : files) {
        std::smatch match;
        if (!std::regex_match(file, match, filePattern)) {
            continue;
        }

        uint32_t partitionId;
        try {
            partitionId = static_cast<uint32_t>(std::stoul(match[1].str()));
        } catch (...) {
            continue;
        }

        TemporalStore store(graphId, partitionId, timeThreshold, edgeThreshold,
                            SnapshotManager::SnapshotMode::HYBRID);
        std::string filePath = snapshotDir + "/" + file;
        if (!store.loadBitmapIndexFromDisk(filePath)) {
            continue;
        }

        partitionsLoaded++;
        auto partitionEdges = store.getEdgesAtSnapshot(snapshotId);
        for (const auto& edge : partitionEdges) {
            uniqueEdges.insert({edge.sourceId, edge.destId});
        }
    }

    if (partitionsLoaded == 0) {
        errorMessage = "No bitmap index files found for graph " + std::to_string(graphId);
        return false;
    }

    edges.assign(uniqueEdges.begin(), uniqueEdges.end());
    return true;
}

bool executeTemporalSnapshotCypher(int connFd,
                                   int graphId,
                                   uint32_t snapshotId,
                                   const std::string& queryString,
                                   std::string& errorMessage) {
    std::vector<std::pair<std::string, std::string>> edges;
    size_t partitionsLoaded = 0;
    if (!loadBitmapFilesForGraphAtSnapshot(graphId, snapshotId, edges, partitionsLoaded, errorMessage)) {
        return false;
    }

    TemporalQueryPlan plan;
    if (!buildTemporalQueryPlan(queryString, plan, errorMessage)) {
        return false;
    }

    json summary;
    summary["type"] = "temporal_snapshot";
    summary["graphId"] = graphId;
    summary["snapshotId"] = snapshotId;
    summary["partitionsLoaded"] = partitionsLoaded;
    summary["edgeCount"] = edges.size();
    summary["queryType"] = plan.isCountQuery ? "count" : "match";

    if (!writeLineToSocket(connFd, summary.dump())) {
        errorMessage = "Error writing temporal snapshot summary to socket";
        return false;
    }

    return writeTemporalQueryRows(connFd, plan, edges, "snapshotId", snapshotId, errorMessage);
}

bool executeTemporalRangeCypher(int connFd,
                                int graphId,
                                uint32_t startSnapshot,
                                uint32_t endSnapshot,
                                const std::string& queryString,
                                std::string& errorMessage) {
    std::set<std::pair<std::string, std::string>> allUniqueEdges;
    size_t snapshotsLoaded = 0;

    for (uint32_t snapshotId = startSnapshot; snapshotId <= endSnapshot; ++snapshotId) {
        std::vector<std::pair<std::string, std::string>> edges;
        size_t partitionsLoaded = 0;
        std::string snapshotError;
        if (!loadBitmapFilesForGraphAtSnapshot(graphId, snapshotId, edges, partitionsLoaded, snapshotError)) {
            continue;
        }

        snapshotsLoaded++;
        for (const auto& edge : edges) {
            allUniqueEdges.insert(edge);
        }
    }

    if (snapshotsLoaded == 0) {
        errorMessage = "No temporal bitmap snapshots loaded for graph " + std::to_string(graphId);
        return false;
    }

    std::vector<std::pair<std::string, std::string>> edges(allUniqueEdges.begin(), allUniqueEdges.end());

    TemporalQueryPlan plan;
    if (!buildTemporalQueryPlan(queryString, plan, errorMessage)) {
        return false;
    }

    json summary;
    summary["type"] = "temporal_range";
    summary["graphId"] = graphId;
    summary["startSnapshot"] = startSnapshot;
    summary["endSnapshot"] = endSnapshot;
    summary["snapshotsLoaded"] = snapshotsLoaded;
    summary["edgeCount"] = edges.size();
    summary["queryType"] = plan.isCountQuery ? "count" : "match";

    if (!writeLineToSocket(connFd, summary.dump())) {
        errorMessage = "Error writing temporal range summary to socket";
        return false;
    }

    json snapshotRange = {startSnapshot, endSnapshot};
    return writeTemporalQueryRows(connFd, plan, edges, "snapshotRange", snapshotRange, errorMessage);
}

}  // namespace

inline const json* getNestedValuePtr(const json& obj, const std::string& dottedKey) {
    // Direct lookup first
    auto it = obj.find(dottedKey);
    if (it != obj.end()) {
        return &(*it);
    }

    // Nested resolution
    const json* current = &obj;
    size_t start = 0;
    while (start < dottedKey.size()) {
        size_t dot = dottedKey.find('.', start);
        std::string key = dottedKey.substr(start, dot - start);
        if (!current->is_object()) {
            cypher_logger.error("Current JSON is not an object at key: '" + key + "'");
            return nullptr;
        }
        auto nested = current->find(key);
        if (nested == current->end()) {
            cypher_logger.error("Key '" + key + "' not found");
            return nullptr;
        }
        current = &(*nested);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return current;
}

CypherQueryExecutor::CypherQueryExecutor() {}

CypherQueryExecutor::CypherQueryExecutor(SQLiteDBInterface *db, PerformanceSQLiteDBInterface *perfDb,
    JobRequest jobRequest) {
    this->sqlite = db;
    this->perfDB = perfDb;
    this->request = jobRequest;
}

void CypherQueryExecutor::execute() {
    // Start automatic OpenTelemetry tracing for Cypher query execution
    OTEL_TRACE_FUNCTION();

    cypher_logger.info("Executing Cypher Query");

    int uniqueId = getUid();
    std::string masterIP = request.getMasterIP();
    std::string graphId = request.getParameter(Conts::PARAM_KEYS::GRAPH_ID);
    std::string canCalibrateString = request.getParameter(Conts::PARAM_KEYS::CAN_CALIBRATE);
    std::string autoCalibrateString = request.getParameter(Conts::PARAM_KEYS::AUTO_CALIBRATION);
    std::string queueTime = request.getParameter(Conts::PARAM_KEYS::QUEUE_TIME);
    std::string graphSLAString = request.getParameter(Conts::PARAM_KEYS::GRAPH_SLA);
    std::string queryString = request.getParameter(Conts::PARAM_KEYS::CYPHER_QUERY::QUERY_STRING);
    int numberOfPartitions = std::stoi(request.getParameter(Conts::PARAM_KEYS::NO_OF_PARTITIONS));
    int connFd = std::stoi(request.getParameter(Conts::PARAM_KEYS::CONN_FILE_DESCRIPTOR));
    bool *loop_exit = reinterpret_cast<bool*>(static_cast<std::uintptr_t>(std::stoull(
        request.getParameter(Conts::PARAM_KEYS::LOOP_EXIT_POINTER))));


    bool canCalibrate = Utils::parseBoolean(canCalibrateString);
    bool autoCalibrate = Utils::parseBoolean(autoCalibrateString);
    std::string commandType = request.getJobType();
    bool isTemporalCypherCommand = (commandType == TMP_CYPHER);

    auto begin = chrono::high_resolution_clock::now();

    auto finalizeExecution = [&](long long msDuration) {
        workerResponded = true;
        JobResponse jobResponse;
        jobResponse.setJobId(request.getJobId());
        responseVector.push_back(jobResponse);

        responseVectorMutex.lock();
        responseMap[request.getJobId()] = jobResponse;
        responseVectorMutex.unlock();

        if (canCalibrate || autoCalibrate) {
            Utils::updateSLAInformation(perfDB, graphId, numberOfPartitions, msDuration, CYPHER,
                                        Conts::SLA_CATEGORY::LATENCY);
            isStatCollect = false;
        }

        processStatusMutex.lock();
        for (auto processCompleteIterator = processData.begin(); processCompleteIterator != processData.end();
             ++processCompleteIterator) {
            ProcessInfo processInformation = *processCompleteIterator;
            if (processInformation.id == uniqueId) {
                processData.erase(processInformation);
                break;
            }
        }
        processStatusMutex.unlock();
    };

    uint32_t snapshotId = 0;
    uint32_t startSnapshot = 0;
    uint32_t endSnapshot = 0;
    int graphIdInt = 0;
    try {
        graphIdInt = std::stoi(graphId);
    } catch (...) {
        graphIdInt = 0;
    }

    if (isTemporalCypherCommand && graphIdInt > 0 && tryParseTemporalSnapshotClause(queryString, snapshotId)) {
        std::string temporalError;
        bool temporalSuccess = executeTemporalSnapshotCypher(connFd, graphIdInt, snapshotId, queryString,
                                     temporalError);
        if (!temporalSuccess) {
            writeLineToSocket(connFd, "{\"error\":\"" + temporalError + "\"}");
            cypher_logger.error("Temporal Cypher snapshot query failed: " + temporalError);
        } else {
            cypher_logger.info("Temporal Cypher snapshot query completed for graph " + graphId +
                               " snapshot " + std::to_string(snapshotId));
        }

        auto end = chrono::high_resolution_clock::now();
        auto dur = end - begin;
        auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        finalizeExecution(msDuration);
        return;
    }

    if (isTemporalCypherCommand && graphIdInt > 0 &&
        tryParseTemporalRangeClause(queryString, startSnapshot, endSnapshot)) {
        std::string temporalError;
        bool temporalSuccess = executeTemporalRangeCypher(connFd, graphIdInt, startSnapshot, endSnapshot,
                                  queryString,
                                                          temporalError);
        if (!temporalSuccess) {
            writeLineToSocket(connFd, "{\"error\":\"" + temporalError + "\"}");
            cypher_logger.error("Temporal Cypher range query failed: " + temporalError);
        } else {
            cypher_logger.info("Temporal Cypher range query completed for graph " + graphId +
                               " range [" + std::to_string(startSnapshot) + ", " +
                               std::to_string(endSnapshot) + "]");
        }

        auto end = chrono::high_resolution_clock::now();
        auto dur = end - begin;
        auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        finalizeExecution(msDuration);
        return;
    }

    if (isTemporalCypherCommand) {
        std::string errorMessage =
            "tmpcp queries must include 'AT SNAPSHOT <id>' or 'FROM <start> TO <end>'";
        writeLineToSocket(connFd, "{\"error\":\"" + errorMessage + "\"}");
        cypher_logger.error("Temporal Cypher query rejected: missing temporal clause");

        auto end = chrono::high_resolution_clock::now();
        auto dur = end - begin;
        auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
        finalizeExecution(msDuration);
        return;
    }

    // Add query attributes for tracing
    OpenTelemetryUtil::addSpanAttribute("graph.id", graphId);
    OpenTelemetryUtil::addSpanAttribute("query.type", "cypher");
    OpenTelemetryUtil::addSpanAttribute("partition.count", std::to_string(numberOfPartitions));

    string queryPlan;
    {
        OTEL_TRACE_OPERATION("parse_and_plan_query");

        antlr4::ANTLRInputStream input(queryString);
        // Create a lexer from the input
        CypherLexer lexer(&input);
        cypher_logger.info("Created lexer from input");

        // Create a token stream from the lexer
        antlr4::CommonTokenStream tokens(&lexer);
        cypher_logger.info("Created tokens from lexer");

        // Create a parser from the token stream
        CypherParser parser(&tokens);
        cypher_logger.info("Created parser from tokens");

        ASTBuilder astBuilder;
        auto* ast = any_cast<ASTNode*>(astBuilder.visitOC_Cypher(parser.oC_Cypher()));

        SemanticAnalyzer semanticAnalyzer;
        if (semanticAnalyzer.analyze(ast)) {
            cypher_logger.info("AST is successfully analyzed");
            QueryPlanner queryPlanner;
            Operator *executionPlan = queryPlanner.createExecutionPlan(ast);
            queryPlan = executionPlan->execute();
        } else {
            cypher_logger.error("Query isn't semantically correct: " + queryString);
        }
    }

    std::vector<std::future<void>> intermRes;
    std::vector<std::future<int>> statResponse;

    // Capture the master trace context for all workers
    std::string masterTraceContext = OpenTelemetryUtil::getCurrentTraceContext();

    const auto &workerList = JasmineGraphServer::getWorkers(numberOfPartitions);

    std::vector<std::unique_ptr<SharedBuffer>> bufferPool;
    bufferPool.reserve(numberOfPartitions);  // Pre-allocate space for pointers
    for (size_t i = 0; i < numberOfPartitions; ++i) {
        bufferPool.emplace_back(std::make_unique<SharedBuffer>(MASTER_BUFFER_SIZE));
    }

    std::vector<std::thread> workerThreads;
    int count = 0;
    {
        OTEL_TRACE_OPERATION("distribute_to_workers");

        for (auto worker : workerList) {
            {
                OTEL_TRACE_OPERATION("send_to_worker_" + std::string(worker.hostname) +
                                     "_partition_" + std::to_string(count));

                workerThreads.emplace_back(
                    doCypherQuery,
                    worker.hostname, worker.port,
                    masterIP, std::stoi(graphId), count,
                    queryPlan, std::ref(*bufferPool[count]), masterTraceContext);
            }
            count++;
        }
    }

    PerformanceUtil::init();

    std::string query =
        "SELECT attempt from graph_sla INNER JOIN sla_category where graph_sla.id_sla_category=sla_category.id and "
        "graph_sla.graph_id='" +
        graphId + "' and graph_sla.partition_count='" + std::to_string(numberOfPartitions) +
        "' and sla_category.category='" + Conts::SLA_CATEGORY::LATENCY + "' and sla_category.command='" + CYPHER +
        "';";

    std::vector<vector<pair<string, string>>> queryResults = perfDB->runSelect(query);

    if (queryResults.size() > 0) {
        std::string attemptString = queryResults[0][0].second;
        int calibratedAttempts = atoi(attemptString.c_str());

        if (calibratedAttempts >= Conts::MAX_SLA_CALIBRATE_ATTEMPTS) {
            canCalibrate = false;
        }
    } else {
        cypher_logger.info("###CYPHER-QUERY-EXECUTOR### Inserting initial record for SLA ");
        Utils::updateSLAInformation(perfDB, graphId, numberOfPartitions, 0, CYPHER, Conts::SLA_CATEGORY::LATENCY);
        statResponse.push_back(std::async(std::launch::async, AbstractExecutor::collectPerformaceData, perfDB,
                                          graphId.c_str(), CYPHER, Conts::SLA_CATEGORY::LATENCY, numberOfPartitions,
                                          masterIP, autoCalibrate));
        isStatCollect = true;
    }

    int result_wr;
    int closeFlag = 0;
    if (Operator::isAggregate) {
        OTEL_TRACE_OPERATION("aggregate_results");
        std::string aggregationType = "unknown";
        if (Operator::aggregateType == AggregationFactory::AVERAGE) {
            aggregationType = "average";
        } else if (Operator::aggregateType == AggregationFactory::ASC) {
            aggregationType = "ascending";
        } else if (Operator::aggregateType == AggregationFactory::DESC) {
            aggregationType = "descending";
        }
        OpenTelemetryUtil::addSpanAttribute("aggregation.type", aggregationType);

        auto startTime = std::chrono::high_resolution_clock::now();
        if (Operator::aggregateType == AggregationFactory::AVERAGE) {
            OTEL_TRACE_OPERATION("average_aggregation");
            Aggregation* aggregation = AggregationFactory::getAggregationMethod(AggregationFactory::AVERAGE);
            while (true) {
                if (closeFlag == numberOfPartitions) {
                    break;
                }
                for (size_t i = 0; i < bufferPool.size(); ++i) {
                    std::string data;
                    if (bufferPool[i]->tryGet(data)) {
                        if (data == "-1") {
                            closeFlag++;
                        } else {
                            aggregation->insert(data);
                        }
                    }
                }
            }
            aggregation->getResult(connFd);
        } else if (Operator::aggregateType == AggregationFactory::ASC ||
                   Operator::aggregateType == AggregationFactory::DESC) {
            OTEL_TRACE_OPERATION("order_by_aggregation");

            struct BufferEntry {
                std::string value;
                size_t bufferIndex;
                json data;
                bool isAsc;
                BufferEntry(const std::string& v, size_t idx, const json& parsed, bool asc)
                        : value(v), bufferIndex(idx), data(parsed), isAsc(asc) {}
                bool operator<(const BufferEntry& other) const {
                    const json* val1 = getNestedValuePtr(data, Operator::aggregateKey);
                    if (!val1) {
                        cypher_logger.error("Missing key in val1 for comparison: " + Operator::aggregateKey);
                        return false;  // or decide what fallback you want
                    }
                    const json* val2 = getNestedValuePtr(other.data, Operator::aggregateKey);
                    if (!val2) {
                        cypher_logger.error("Missing key in val2 for comparison: " + Operator::aggregateKey);
                        return false;
                    }
                    bool result;
                    if (val1->is_number_integer() && val2->is_number_integer()) {
                        result = val1->get<int>() > val2->get<int>();
                    } else if (val1->is_string() && val2->is_string()) {
                        result = val1->get<std::string>() > val2->get<std::string>();
                    } else {
                        result = val1->dump() > val2->dump();  // fallback comparison
                    }
                    return isAsc ? result : !result;
                }
            };
            bool isAsc = (Operator::aggregateType == AggregationFactory::ASC);
            auto cmp = [](const BufferEntry& a, const BufferEntry& b) { return a < b; };
            std::priority_queue<BufferEntry, std::vector<BufferEntry>, decltype(cmp)> mergeQueue(cmp);

            cypher_logger.info("START MASTER STREAMING MERGE");
            for (size_t i = 0; i < bufferPool.size(); ++i) {
                std::string value = bufferPool[i]->get();
                if (!value.empty()) {
                    if (value == "-1") {
                        closeFlag++;
                    } else {
                        try {
                            json parsed = json::parse(value);
                            BufferEntry entry{value, i, parsed, isAsc};
                            mergeQueue.push(entry);
                        } catch (const json::exception &e) {
                            cypher_logger.error("JSON parse error in init: " + std::string(e.what()));
                        }
                    }
                }
            }

            // Streaming merge loop
            while (!mergeQueue.empty()) {
                BufferEntry top = mergeQueue.top();
                mergeQueue.pop();
                result_wr = write(connFd, top.value.c_str(), top.value.length());
                write(connFd, Conts::CARRIAGE_RETURN_NEW_LINE.c_str(), Conts::CARRIAGE_RETURN_NEW_LINE.size());

                // Try to get next element from the same worker buffer
                std::string nextValue = bufferPool[top.bufferIndex]->get();
                if (nextValue == "-1") {
                    closeFlag++;
                } else {
                    try {
                        json parsed = json::parse(nextValue);
                        BufferEntry nextEntry{nextValue, top.bufferIndex, parsed, isAsc};
                        mergeQueue.push(nextEntry);
                    } catch (const json::exception& e) {
                        cypher_logger.error("JSON parse error: " + std::string(e.what()));
                    }
                }
                if (closeFlag >= bufferPool.size()) break;
            }
            cypher_logger.info("MASTER STREAMING MERGE COMPLETED");

        } else {
            std::string log = "Query is recongnized as Aggreagation, but method doesnot have implemented yet";
            result_wr = write(connFd, log.c_str(), log.length());
            result_wr = write(connFd, Conts::CARRIAGE_RETURN_NEW_LINE.c_str(),
                              Conts::CARRIAGE_RETURN_NEW_LINE.size());
            if (result_wr < 0) {
                cypher_logger.error("Error writing to socket");
                *loop_exit = true;
                return;
            }
        }
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        int totalTime = duration.count();
        cypher_logger.info("Total time taken for aggregation: " + std::to_string(totalTime) + " ms");
        Operator::isAggregate = false;
    } else {
        OTEL_TRACE_OPERATION("collect_non_aggregated_results");

        int count = 0;
        while (true) {
            if (closeFlag == numberOfPartitions) {
                break;
            }
            for (size_t i = 0; i < bufferPool.size(); ++i) {
                std::string data;
                if (bufferPool[i]->tryGet(data)) {
                    if (data == "-1") {
                        closeFlag++;
                    } else {
                        count++;
                        result_wr = write(connFd, data.c_str(), data.length());
                        result_wr = write(connFd, Conts::CARRIAGE_RETURN_NEW_LINE.c_str(),
                                          Conts::CARRIAGE_RETURN_NEW_LINE.size());
                        if (result_wr < 0) {
                            cypher_logger.error("Error writing to socket");
                            *loop_exit = true;
                            return;
                        }
                    }
                }
            }
        }
        cypher_logger.info("Total records returned: " + std::to_string(count));
    }

    for (auto& thread : workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    cypher_logger.info("###CYPHER-QUERY-EXECUTOR### Executing Query : Completed");

    auto end = chrono::high_resolution_clock::now();
    auto dur = end - begin;
    auto msDuration = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    finalizeExecution(msDuration);
}

void CypherQueryExecutor::doCypherQuery(const std::string& host, int port, const std::string& masterIP, int graphID,
                                               int PartitionId, const std::string& message, SharedBuffer &sharedBuffer,
                                               const std::string& masterTraceContext) {
    OTEL_TRACE_OPERATION("worker_communication_" + host + "_partition_" + std::to_string(PartitionId));

    Utils::sendQueryPlanToWorker(host, port, masterIP, graphID, PartitionId, message, sharedBuffer, masterTraceContext);
}


int CypherQueryExecutor::getUid() {
    static std::atomic<std::uint32_t> uid{0};
    return ++uid;
}

