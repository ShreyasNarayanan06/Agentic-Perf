#include "model.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace perf;

namespace {

void usage() {
    std::cout << "Usage: architecture_perf [options]\n"
        << "Policy-neutral, discrete-event sensitivity model. Times are simulated.\n\n"
        << "Workload: --agents 3 --lead-steps 32 --rounds 64\n"
        << "          --prefix-reads 32 --leaf-writes 8 --metadata-ops 2\n"
        << "          --compute-ns 200 --host-compute-ns 50\n"
        << "System:   --outstanding 8 --memory-ns 150 --bandwidth-gbps 32\n"
        << "Assumed generic coherence overhead per off-chip transaction:\n"
        << "          --coherence-ns 40 --coherence-bytes 32\n"
        << "Optional cost paid only by hypothetical alternatives on KV requests:\n"
        << "          --added-latency-ns 0 --setup-ns 0\n"
        << "Output:   --csv metrics.csv --phases phase_timings.csv --help\n\n"
        << "Default comparisons retain 100/75/50/25/0% of KV overhead, plus separate\n"
        << "latency-only and traffic-only reductions. Metadata costs remain fixed.\n";
}

double real(const std::string& value) {
    std::size_t end = 0;
    const auto number = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(number) || number < 0)
        throw std::invalid_argument("expected finite nonnegative number: " + value);
    return number;
}

std::size_t integer(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected nonnegative integer: " + value);
    const auto number = std::stoull(value);
    if (number > std::numeric_limits<std::size_t>::max()) throw std::out_of_range("integer too large");
    return static_cast<std::size_t>(number);
}

double ratio(double numerator, double denominator) {
    return denominator == 0 ? 0 : numerator / denominator;
}

double speedup(const Result& baseline, const Result& result) {
    return result.execution_ns == 0 ? 1 : baseline.execution_ns / result.execution_ns;
}

std::ofstream file(const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot open output: " + path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    return out;
}

void csv(std::ostream& out, const std::vector<Result>& results, const Config& config) {
    out << "scenario,latency_retained,traffic_retained,execution_us,speedup,execution_reduction_pct,"
           "critical_path_saved_ns,requests,metadata_requests,prefix_requests,leaf_requests,"
           "data_bytes,control_bytes,total_link_bytes,link_utilization_pct,avg_queue_wait_ns,"
           "aggregate_worker_compute_ns,aggregate_worker_memory_wait_ns,"
           "aggregate_coherence_latency_ns,aggregate_added_latency_ns,peak_inflight,"
           "agents,lead_steps,rounds,prefix_reads_per_step,leaf_writes_per_step,metadata_ops_per_step,"
           "outstanding,compute_ns,host_compute_ns,memory_ns,bandwidth_gbps,coherence_ns,coherence_bytes,"
           "added_latency_per_kv_ns,setup_ns\n";
    out << std::fixed << std::setprecision(6);
    const auto& baseline = results.front();
    for (const auto& r : results)
        out << r.scenario.label << ',' << r.scenario.latency_retained << ',' << r.scenario.traffic_retained
            << ',' << r.execution_ns / 1000 << ',' << speedup(baseline, r) << ','
            << 100 * ratio(baseline.execution_ns - r.execution_ns, baseline.execution_ns) << ','
            << baseline.execution_ns - r.execution_ns << ',' << r.requests << ','
            << r.metadata_requests << ',' << r.prefix_requests << ',' << r.leaf_requests << ','
            << r.data_bytes << ',' << r.control_bytes << ',' << r.data_bytes + r.control_bytes
            << ',' << 100 * ratio(r.link_busy_ns, r.execution_ns) << ','
            << ratio(r.queue_wait_ns, static_cast<double>(r.requests)) << ','
            << r.worker_compute_ns << ',' << r.worker_memory_wait_ns << ','
            << r.assumed_coherence_latency_ns << ',' << r.added_latency_ns << ',' << r.peak_inflight << ','
            << config.agents << ',' << config.lead_steps << ',' << config.rounds << ',' << config.prefix_reads
            << ',' << config.leaf_writes << ',' << config.metadata_ops << ',' << config.outstanding
            << ',' << config.compute_ns << ',' << config.host_compute_ns << ',' << config.memory_ns
            << ',' << config.bandwidth_gbps << ',' << config.coherence_ns << ',' << config.coherence_bytes
            << ',' << config.added_latency_ns << ',' << config.setup_ns << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        Config config;
        std::string csv_path, phases_path;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") { usage(); return 0; }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--csv") csv_path = value;
            else if (option == "--phases") phases_path = value;
            else if (option == "--agents") config.agents = integer(value);
            else if (option == "--lead-steps") config.lead_steps = integer(value);
            else if (option == "--rounds") config.rounds = integer(value);
            else if (option == "--prefix-reads") config.prefix_reads = integer(value);
            else if (option == "--leaf-writes") config.leaf_writes = integer(value);
            else if (option == "--metadata-ops") config.metadata_ops = integer(value);
            else if (option == "--outstanding") config.outstanding = integer(value);
            else if (option == "--compute-ns") config.compute_ns = real(value);
            else if (option == "--host-compute-ns") config.host_compute_ns = real(value);
            else if (option == "--memory-ns") config.memory_ns = real(value);
            else if (option == "--bandwidth-gbps") config.bandwidth_gbps = real(value);
            else if (option == "--coherence-ns") config.coherence_ns = real(value);
            else if (option == "--coherence-bytes") config.coherence_bytes = real(value);
            else if (option == "--added-latency-ns") config.added_latency_ns = real(value);
            else if (option == "--setup-ns") config.setup_ns = real(value);
            else throw std::invalid_argument("unknown option: " + option);
        }
        config.validate();
        if (!csv_path.empty() && csv_path == phases_path)
            throw std::invalid_argument("metrics and phase outputs require distinct paths");
        const auto workload = make_workload(config);
        std::vector<Result> results;
        for (const auto& scenario : default_scenarios())
            results.push_back(simulate(workload, config, scenario));
        const auto& reference = results.front();
        std::cout << "Policy-neutral multi-agent performance sensitivity model\n"
                  << "Host + " << config.agents << " accelerators; memory-level parallelism " << config.outstanding
                  << "; bandwidth " << config.bandwidth_gbps << " GB/s\n"
                  << "Assumed per-transaction coherence tax: " << config.coherence_ns << " ns + "
                  << config.coherence_bytes << " control bytes\n"
                  << "Same " << reference.requests << " off-chip transactions and "
                  << reference.data_bytes << " data bytes in every comparison.\n\n";
        std::cout << std::left << std::setw(30) << "Scenario" << std::right
                  << std::setw(13) << "Time (us)" << std::setw(12) << "Speedup"
                  << std::setw(12) << "Link busy%" << std::setw(15) << "Queue avg ns"
                  << std::setw(15) << "Control KiB" << '\n';
        for (const auto& r : results)
            std::cout << std::left << std::setw(30) << r.scenario.label << std::right << std::fixed
                      << std::setprecision(3) << std::setw(13) << r.execution_ns / 1000
                      << std::setw(12) << speedup(reference, r)
                      << std::setw(12) << 100 * ratio(r.link_busy_ns, r.execution_ns)
                      << std::setw(15) << ratio(r.queue_wait_ns, static_cast<double>(r.requests))
                      << std::setw(15) << r.control_bytes / 1024 << '\n';
        const auto& half = results.at(2);
        std::cout << "\nAt 50% lower KV overhead: "
                  << 100 * ratio(reference.execution_ns - half.execution_ns, reference.execution_ns)
                  << "% less simulated execution time; " << reference.execution_ns - half.execution_ns
                  << " ns removed from the critical path.\n"
                  << "KV reduction is a hypothetical input; metadata overhead stays unchanged.\n"
                  << "Times include compute/memory overlap and link queueing. No coherence policy is implemented.\n"
                  << "These are sensitivity estimates from assumed costs, not measured hardware speedups.\n";
        if (!csv_path.empty()) { auto out = file(csv_path); csv(out, results, config); }
        if (!phases_path.empty()) {
            auto out = file(phases_path);
            out << "scenario,phase,start_ns,end_ns,duration_ns\n" << std::fixed << std::setprecision(6);
            for (const auto& r : results)
                for (const auto& p : r.phases)
                    out << r.scenario.label << ',' << p.label << ',' << p.start_ns << ','
                        << p.end_ns << ',' << p.end_ns - p.start_ns << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
