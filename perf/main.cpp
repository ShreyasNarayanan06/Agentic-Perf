#include "model.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace perf;
namespace {
double real(const std::string& text) {
    std::size_t end = 0;
    const auto n = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(n) || n < 0) throw std::invalid_argument("invalid nonnegative number: " + text);
    return n;
}
std::size_t integer(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid integer: " + text);
    const auto n = std::stoull(text);
    if (n > std::numeric_limits<std::size_t>::max()) throw std::out_of_range("integer too large");
    return static_cast<std::size_t>(n);
}
double ratio(double a, double b) { return b == 0 ? 0 : a / b; }
std::ofstream file(const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open " + path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::fixed << std::setprecision(9);
    return out;
}
using Fields = std::vector<std::pair<std::string, double>>;
Fields inputs(const Config& c) {
    return {{"agents", static_cast<double>(c.agents)}, {"lead_steps", static_cast<double>(c.lead_steps)},
        {"rounds", static_cast<double>(c.rounds)}, {"prefix_reads", static_cast<double>(c.prefix_reads)},
        {"leaf_writes", static_cast<double>(c.leaf_writes)}, {"metadata_ops", static_cast<double>(c.metadata_ops)},
        {"prefix_lines", static_cast<double>(c.prefix_lines)}, {"leaf_lines", static_cast<double>(c.leaf_lines)},
        {"stores_per_line", static_cast<double>(c.stores_per_line)}, {"cache_bytes", static_cast<double>(c.cache_bytes)},
        {"outstanding", static_cast<double>(c.outstanding)}, {"compute_ns", c.compute_ns}, {"host_compute_ns", c.host_compute_ns},
        {"hit_ns", c.hit_ns}, {"memory_ns", c.memory_ns}, {"peer_ns", c.peer_ns}, {"bandwidth_gbps", c.bandwidth_gbps},
        {"directory_ns", c.directory_ns}, {"snoop_ns", c.snoop_ns}, {"invalidate_ns", c.invalidate_ns}, {"writeback_ns", c.writeback_ns},
        {"directory_bytes", c.directory_bytes}, {"snoop_bytes", c.snoop_bytes}, {"invalidate_bytes", c.invalidate_bytes},
        {"overlap_trigger", c.overlap_trigger}, {"fork_barrier_ns", c.fork_barrier_ns}, {"region_barrier_ns", c.region_barrier_ns},
        {"setup_ns", c.setup_ns}, {"region_bytes", static_cast<double>(c.region_bytes)}, {"flush_at_fork", c.flush_at_fork ? 1.0 : 0.0},
        {"recovery_rate", c.recovery_rate}, {"recovery_ns", c.recovery_ns}, {"recovery_bytes", c.recovery_bytes},
        {"added_latency_per_kv_access_ns", c.added_latency_ns}};
}
void write_csv(std::ostream& out, const std::vector<Result>& results, const Config& c) {
    out << "scenario,latency_retained,traffic_retained,execution_us,speedup,execution_reduction_pct,logical_accesses,"
           "offchip_requests,cache_hits,fills,directory_lookups,snoops,invalidations,evictions,dirty_evictions,"
           "writebacks_including_fork,fill_bytes,writeback_bytes,control_bytes,total_link_bytes,link_utilization_pct,"
           "aggregate_queue_wait_ns,aggregate_worker_compute_ns,aggregate_worker_memory_wait_ns,aggregate_overlap_ns,"
           "aggregate_event_latency_ns,aggregate_added_latency_ns,expected_recoveries,fork_flushes,fork_penalty_ns,prefix_regions,peak_inflight";
    for (const auto& field : inputs(c)) out << ',' << field.first;
    out << '\n';
    for (const auto& r : results) {
        Traffic t; for (const auto& kind : r.kinds) t += kind;
        out << r.scenario.label << ',' << r.scenario.latency_retained << ',' << r.scenario.traffic_retained << ','
            << r.execution_ns / 1000 << ',' << (r.execution_ns == 0 ? 1 : results.front().execution_ns / r.execution_ns) << ','
            << 100 * ratio(results.front().execution_ns - r.execution_ns, results.front().execution_ns) << ','
            << r.requests << ',' << r.offchip_requests << ',' << t.hits << ',' << t.fills << ',' << t.directory << ','
            << t.snoops << ',' << t.invalidations << ',' << t.evictions << ',' << t.dirty_evictions << ','
            << t.writebacks + r.fork_flushes << ',' << r.data_bytes << ',' << r.writeback_bytes << ',' << r.control_bytes << ','
            << r.data_bytes + r.writeback_bytes + r.control_bytes << ',' << 100 * ratio(r.link_busy_ns, r.execution_ns) << ','
            << r.queue_wait_ns << ',' << r.worker_compute_ns << ',' << r.worker_memory_wait_ns << ',' << r.overlap_ns << ','
            << r.assumed_coherence_latency_ns << ',' << r.added_latency_ns << ',' << r.expected_recoveries << ','
            << r.fork_flushes << ',' << r.fork_penalty_ns << ',' << r.prefix_regions << ',' << r.peak_inflight;
        for (const auto& field : inputs(c)) out << ',' << field.second;
        out << '\n';
    }
}
void usage() {
    std::cout << "Usage: architecture_perf [options]\n"
        << "Finite-cache, trace-profiled performance sensitivity model; no optimized policy.\n"
        << "Workload: --agents 3 --lead-steps 32 --rounds 64 --prefix-reads 32 --leaf-writes 8 --metadata-ops 2\n"
        << "          --prefix-lines 64 --leaf-lines 128 --stores-per-line 4\n"
        << "Timing: --cache-bytes 65536 --hit-ns 2 --outstanding 8 --overlap-trigger 0.5\n"
        << "        --compute-ns 200 --host-compute-ns 50 --memory-ns 150 --peer-ns 150 --bandwidth-gbps 32\n"
        << "Baseline event costs: --directory-ns 20 --snoop-ns 20 --invalidate-ns 12 --writeback-ns 10\n"
        << "                      --directory-bytes 16 --snoop-bytes 16 --invalidate-bytes 16\n"
        << "Candidate budgets: --fork-barrier-ns 200 --region-barrier-ns 40 --region-bytes 4096\n"
        << "                   --setup-ns 0 --flush-at-fork 1 --recovery-rate 0.01\n"
        << "                   --recovery-ns 200 --recovery-bytes 32 --added-latency-ns 0\n"
        << "Output: --csv metrics.csv --kinds access_kinds.csv --phases phases.csv\n"
        << "Cache bytes 0 disables retention. Overlap trigger 1 disables within-agent overlap.\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        Config c;
        std::string csv_path, kinds_path, phases_path;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") { usage(); return 0; }
            if (i + 1 == argc) throw std::invalid_argument("missing value for " + arg);
            const std::string v = argv[++i];
            if (arg == "--csv") csv_path = v;
            else if (arg == "--kinds") kinds_path = v;
            else if (arg == "--phases") phases_path = v;
            else if (arg == "--agents") c.agents = integer(v);
            else if (arg == "--lead-steps") c.lead_steps = integer(v);
            else if (arg == "--rounds") c.rounds = integer(v);
            else if (arg == "--prefix-reads") c.prefix_reads = integer(v);
            else if (arg == "--leaf-writes") c.leaf_writes = integer(v);
            else if (arg == "--metadata-ops") c.metadata_ops = integer(v);
            else if (arg == "--prefix-lines") c.prefix_lines = integer(v);
            else if (arg == "--leaf-lines") c.leaf_lines = integer(v);
            else if (arg == "--stores-per-line") c.stores_per_line = integer(v);
            else if (arg == "--cache-bytes") c.cache_bytes = integer(v);
            else if (arg == "--region-bytes") c.region_bytes = integer(v);
            else if (arg == "--outstanding") c.outstanding = integer(v);
            else if (arg == "--compute-ns") c.compute_ns = real(v);
            else if (arg == "--host-compute-ns") c.host_compute_ns = real(v);
            else if (arg == "--hit-ns") c.hit_ns = real(v);
            else if (arg == "--memory-ns") c.memory_ns = real(v);
            else if (arg == "--peer-ns") c.peer_ns = real(v);
            else if (arg == "--bandwidth-gbps") c.bandwidth_gbps = real(v);
            else if (arg == "--directory-ns") c.directory_ns = real(v);
            else if (arg == "--snoop-ns") c.snoop_ns = real(v);
            else if (arg == "--invalidate-ns") c.invalidate_ns = real(v);
            else if (arg == "--writeback-ns") c.writeback_ns = real(v);
            else if (arg == "--directory-bytes") c.directory_bytes = real(v);
            else if (arg == "--snoop-bytes") c.snoop_bytes = real(v);
            else if (arg == "--invalidate-bytes") c.invalidate_bytes = real(v);
            else if (arg == "--overlap-trigger") c.overlap_trigger = real(v);
            else if (arg == "--fork-barrier-ns") c.fork_barrier_ns = real(v);
            else if (arg == "--region-barrier-ns") c.region_barrier_ns = real(v);
            else if (arg == "--setup-ns") c.setup_ns = real(v);
            else if (arg == "--flush-at-fork") { const auto n = integer(v); if (n > 1) throw std::invalid_argument("flush-at-fork must be 0 or 1"); c.flush_at_fork = n != 0; }
            else if (arg == "--recovery-rate") c.recovery_rate = real(v);
            else if (arg == "--recovery-ns") c.recovery_ns = real(v);
            else if (arg == "--recovery-bytes") c.recovery_bytes = real(v);
            else if (arg == "--added-latency-ns") c.added_latency_ns = real(v);
            else throw std::invalid_argument("unknown option: " + arg);
        }
        c.validate();
        if ((!csv_path.empty() && (csv_path == kinds_path || csv_path == phases_path)) ||
            (!kinds_path.empty() && kinds_path == phases_path)) throw std::invalid_argument("output paths must be distinct");
        const auto workload = make_workload(c);
        std::vector<Result> results;
        for (const auto& s : default_scenarios()) results.push_back(simulate(workload, c, s));
        std::cout << "Finite-cache performance sensitivity model (v2)\n"
                  << "Cache per node: " << c.cache_bytes << " B; prefix: " << c.prefix_lines * 64
                  << " B; leaf per agent: " << c.leaf_lines * 64 << " B; overlap trigger: " << c.overlap_trigger << '\n'
                  << "Baseline cache/directory events are profiled in fixed logical order; costs remain assumed.\n\n"
                  << std::left << std::setw(30) << "Scenario" << std::right << std::setw(13) << "Time us"
                  << std::setw(12) << "Speedup" << std::setw(14) << "Off-chip req" << std::setw(14) << "Fork tax ns" << '\n';
        for (const auto& r : results)
            std::cout << std::left << std::setw(30) << r.scenario.label << std::right << std::fixed << std::setprecision(3)
                      << std::setw(13) << r.execution_ns / 1000 << std::setw(12) << ratio(results.front().execution_ns, r.execution_ns)
                      << std::setw(14) << r.offchip_requests << std::setw(14) << r.fork_penalty_ns << '\n';
        std::cout << "\nReference event breakdown (writebacks attributed to triggering access):\n";
        for (const auto kind : {AccessKind::Metadata, AccessKind::PrefixKV, AccessKind::LeafKV}) {
            const auto& t = results.front().kinds[index(kind)];
            std::cout << "  " << name(kind) << ": accesses=" << t.accesses << " hits=" << t.hits << " fills=" << t.fills
                      << " directory=" << t.directory << " snoops=" << t.snoops << " invalidations=" << t.invalidations
                      << " writebacks=" << t.writebacks << " evictions=" << t.evictions << '\n';
        }
        std::cout << "\nHalf KV event overhead: " << 100 * ratio(results.front().execution_ns - results[2].execution_ns, results.front().execution_ns)
                  << "% less simulated time. Negative values mean a regression.\n"
                  << "Fork/flush and expected recovery budgets apply to candidates. No optimized coherence policy is implemented.\n";
        if (!csv_path.empty()) { auto out = file(csv_path); write_csv(out, results, c); }
        if (!kinds_path.empty()) {
            auto out = file(kinds_path);
            out << "scenario,kind,accesses,reads,writes,data_hits,fills,directory_lookups,snoops,invalidations,writebacks,evictions,dirty_evictions\n";
            for (const auto& r : results) for (const auto k : {AccessKind::Metadata, AccessKind::PrefixKV, AccessKind::LeafKV}) {
                const auto& t = r.kinds[index(k)];
                out << r.scenario.label << ',' << name(k) << ',' << t.accesses << ',' << t.reads << ',' << t.writes << ','
                    << t.hits << ',' << t.fills << ',' << t.directory << ',' << t.snoops << ',' << t.invalidations << ','
                    << t.writebacks << ',' << t.evictions << ',' << t.dirty_evictions << '\n';
            }
        }
        if (!phases_path.empty()) {
            auto out = file(phases_path);
            out << "scenario,phase,start_ns,end_ns,duration_ns\n";
            for (const auto& r : results) for (const auto& p : r.phases)
                out << r.scenario.label << ',' << p.label << ',' << p.start_ns << ',' << p.end_ns << ',' << p.end_ns - p.start_ns << '\n';
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}
