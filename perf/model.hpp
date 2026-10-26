#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace perf {
enum class AccessKind { Metadata, PrefixKV, LeafKV };
inline std::size_t index(AccessKind kind) { return static_cast<std::size_t>(kind); }
const char* name(AccessKind kind);

struct Config {
    std::size_t agents = 3, lead_steps = 32, rounds = 64;
    std::size_t prefix_reads = 32, leaf_writes = 8, metadata_ops = 2;
    std::size_t prefix_lines = 64, leaf_lines = 128, stores_per_line = 4;
    std::size_t cache_bytes = 64 * 1024, outstanding = 8;
    double compute_ns = 200, host_compute_ns = 50;
    double hit_ns = 2, memory_ns = 150, peer_ns = 150, bandwidth_gbps = 32;
    double directory_ns = 20, snoop_ns = 20, invalidate_ns = 12, writeback_ns = 10;
    double directory_bytes = 16, snoop_bytes = 16, invalidate_bytes = 16;
    double overlap_trigger = .5;
    double fork_barrier_ns = 200, region_barrier_ns = 40, setup_ns = 0;
    std::size_t region_bytes = 4096;
    bool flush_at_fork = true;
    double recovery_rate = .01, recovery_ns = 200, recovery_bytes = 32;
    double added_latency_ns = 0;
    void validate() const;
};
struct Traffic {
    std::uint64_t accesses = 0, reads = 0, writes = 0, hits = 0, fills = 0;
    std::uint64_t directory = 0, snoops = 0, invalidations = 0, writebacks = 0;
    std::uint64_t evictions = 0, dirty_evictions = 0;
    Traffic& operator+=(const Traffic& other);
};
struct Service { Traffic traffic; bool peer_fill = false; };
struct Access {
    AccessKind kind = AccessKind::Metadata;
    std::uint64_t address = 0;
    bool write = false;
    Service service;
};
struct Step { double compute_ns = 0; std::vector<Access> accesses; };
struct Stream { std::size_t node = 0; std::vector<Step> steps; };
struct Phase { std::string label; std::vector<Stream> streams; bool fork_boundary = false; };
using Workload = std::vector<Phase>;
struct Profile {
    Workload workload;
    std::array<Traffic, 3> kinds{};
    std::uint64_t fork_dirty_lines = 0, fork_flushes = 0, prefix_regions = 0;
};
struct Scenario {
    std::string label;
    double latency_retained = 1, traffic_retained = 1;
    bool counterfactual = false;
};
struct PhaseTiming { std::string label; double start_ns = 0, end_ns = 0; };
struct Result {
    Scenario scenario;
    double execution_ns = 0, worker_compute_ns = 0, worker_memory_wait_ns = 0, overlap_ns = 0;
    double link_busy_ns = 0, queue_wait_ns = 0, data_bytes = 0, writeback_bytes = 0, control_bytes = 0;
    double assumed_coherence_latency_ns = 0, added_latency_ns = 0, expected_recoveries = 0;
    double fork_penalty_ns = 0;
    std::uint64_t requests = 0, offchip_requests = 0, fork_flushes = 0, prefix_regions = 0;
    std::size_t peak_inflight = 0;
    std::array<Traffic, 3> kinds{};
    std::vector<PhaseTiming> phases;
};
Workload make_workload(const Config& config);
Profile profile_workload(const Workload& workload, const Config& config, const Scenario& scenario);
std::vector<Scenario> default_scenarios();
Result simulate(const Workload& workload, const Config& config, const Scenario& scenario);
} // namespace perf
