#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace perf {

enum class AccessKind { Metadata, PrefixKV, LeafKV };

struct Config {
    std::size_t agents = 3;
    std::size_t lead_steps = 32;
    std::size_t rounds = 64;
    std::size_t prefix_reads = 32;
    std::size_t leaf_writes = 8;
    std::size_t metadata_ops = 2;
    std::size_t outstanding = 8; // per agent, memory-level parallelism
    double compute_ns = 200;
    double host_compute_ns = 50;
    double memory_ns = 150;
    double bandwidth_gbps = 32; // decimal GB/s = bytes/ns, aggregate shared link
    double coherence_ns = 40;   // assumed generic overhead, not a protocol
    double coherence_bytes = 32;
    double added_latency_ns = 0;
    double setup_ns = 0;
    void validate() const;
};

struct Access {
    AccessKind kind = AccessKind::Metadata;
    std::uint64_t address = 0;
    bool write = false;
};

struct Step {
    double compute_ns = 0;
    std::vector<Access> accesses;
};

struct Stream {
    std::size_t node = 0; // host 0; one accelerator per child agent
    std::vector<Step> steps;
};

struct Phase {
    std::string label;
    std::vector<Stream> streams;
};

using Workload = std::vector<Phase>;

struct Scenario {
    std::string label;
    double latency_retained = 1;
    double traffic_retained = 1;
    bool counterfactual = false;
};

struct PhaseTiming {
    std::string label;
    double start_ns = 0;
    double end_ns = 0;
};

struct Result {
    Scenario scenario;
    double execution_ns = 0;
    double worker_compute_ns = 0;
    double worker_memory_wait_ns = 0;
    double link_busy_ns = 0;
    double queue_wait_ns = 0;
    double data_bytes = 0;
    double control_bytes = 0;
    double assumed_coherence_latency_ns = 0; // sum across requests, overlaps
    double added_latency_ns = 0;
    std::uint64_t requests = 0;
    std::uint64_t metadata_requests = 0;
    std::uint64_t prefix_requests = 0;
    std::uint64_t leaf_requests = 0;
    std::size_t peak_inflight = 0;
    std::vector<PhaseTiming> phases;
};

Workload make_workload(const Config& config);
std::vector<Scenario> default_scenarios();
Result simulate(const Workload& workload, const Config& config, const Scenario& scenario);

} // namespace perf
