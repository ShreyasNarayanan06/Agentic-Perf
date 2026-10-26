#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace perf;

namespace {

void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

void close(double actual, double expected) {
    if (std::abs(actual - expected) > 1e-7 * std::max(1.0, std::abs(expected)))
        throw std::runtime_error("expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

template <class Function> void must_throw(Function function) {
    bool threw = false;
    try { function(); } catch (const std::exception&) { threw = true; }
    require(threw, "expected validation error");
}

Workload single(std::vector<Access> accesses, double compute = 0) {
    return {Phase{"one", {Stream{1, {Step{compute, std::move(accesses)}}}}}};
}

void serial_analytical() {
    Config c;
    c.outstanding = 1;
    c.memory_ns = 100;
    c.coherence_ns = 20;
    c.coherence_bytes = 16;
    c.bandwidth_gbps = 16;
    const auto w = single({{AccessKind::LeafKV, 0, true}, {AccessKind::PrefixKV, 64, false}}, 10);
    const auto baseline = simulate(w, c, {"baseline", 1, 1, false});
    close(baseline.execution_ns, 260); // 10 + 2 * (80/16 + 100 + 20)
    close(baseline.worker_memory_wait_ns, 250);
    close(baseline.link_busy_ns, 10);
    close(baseline.queue_wait_ns, 0);
    const auto half = simulate(w, c, {"half", .5, .5, true});
    close(half.execution_ns, 239); // 10 + 2 * (72/16 + 100 + 10)
}

void concurrency_and_mlp() {
    Config c;
    c.outstanding = 1;
    c.memory_ns = 100;
    c.coherence_ns = 0;
    c.coherence_bytes = 0;
    c.bandwidth_gbps = 64;
    const Step step{0, {{AccessKind::LeafKV, 0, true}}};
    const Workload parallel{{"parallel", {{1, {step}}, {2, {step}}}}};
    const auto result = simulate(parallel, c, {"base", 1, 1, false});
    close(result.execution_ns, 102); // two overlapping 101 ns loads, second issues after 1 ns on link
    close(result.worker_memory_wait_ns, 203); // deliberately not the makespan
    close(result.queue_wait_ns, 1);
    require(result.peak_inflight == 2, "parallel requests did not overlap");
    const auto sequential = single({{AccessKind::LeafKV, 0, true}, {AccessKind::LeafKV, 64, true}});
    close(simulate(sequential, c, {"base", 1, 1, false}).execution_ns, 202);
    c.outstanding = 2;
    close(simulate(sequential, c, {"base", 1, 1, false}).execution_ns, 102);
}

void phase_dependencies() {
    Config c;
    c.memory_ns = 100;
    c.coherence_ns = 0;
    c.coherence_bytes = 0;
    c.bandwidth_gbps = 64;
    auto w = single({{AccessKind::LeafKV, 0, true}});
    w.push_back(w.front());
    const auto result = simulate(w, c, {"base", 1, 1, false});
    close(result.execution_ns, 202);
    close(result.phases[1].start_ns, result.phases[0].end_ns);
}

void unchanged_work_and_metadata() {
    Config c;
    const auto w = make_workload(c);
    const auto baseline = simulate(w, c, default_scenarios().front());
    for (const auto& scenario : default_scenarios()) {
        const auto r = simulate(w, c, scenario);
        require(r.requests == baseline.requests && r.metadata_requests == baseline.metadata_requests &&
                r.prefix_requests == baseline.prefix_requests && r.leaf_requests == baseline.leaf_requests,
                "counterfactual changed workload");
        close(r.data_bytes, baseline.data_bytes);
        close(r.worker_compute_ns, baseline.worker_compute_ns);
        require(r.link_busy_ns <= r.execution_ns, "link utilization over 100%");
        require(r.requests == r.metadata_requests + r.prefix_requests + r.leaf_requests, "request partition");
        close(r.link_busy_ns * c.bandwidth_gbps, r.data_bytes + r.control_bytes);
    }
    const auto ideal = simulate(w, c, {"bound", 0, 0, true});
    close(ideal.control_bytes, static_cast<double>(ideal.metadata_requests) * c.coherence_bytes);
    close(ideal.assumed_coherence_latency_ns, static_cast<double>(ideal.metadata_requests) * c.coherence_ns);
    require(ideal.execution_ns < baseline.execution_ns, "default should have exposed overhead");
}

void zero_overhead_no_speedup() {
    Config c;
    c.coherence_ns = 0;
    c.coherence_bytes = 0;
    const auto w = make_workload(c);
    const auto baseline = simulate(w, c, default_scenarios().front());
    for (const auto& scenario : default_scenarios()) {
        const auto r = simulate(w, c, scenario);
        close(r.execution_ns, baseline.execution_ns);
        close(r.queue_wait_ns, baseline.queue_wait_ns);
    }
}

void metadata_only_no_speedup() {
    Config c;
    c.prefix_reads = 0;
    c.leaf_writes = 0;
    c.added_latency_ns = 1000; // must not apply to metadata
    const auto w = make_workload(c);
    const auto baseline = simulate(w, c, default_scenarios().front());
    for (const auto& scenario : default_scenarios())
        close(simulate(w, c, scenario).execution_ns, baseline.execution_ns);
}

void compute_only() {
    Config c;
    c.prefix_reads = c.leaf_writes = c.metadata_ops = 0;
    c.compute_ns = 10;
    c.host_compute_ns = 0;
    const auto result = simulate(make_workload(c), c, {"base", 1, 1, false});
    close(result.execution_ns, static_cast<double>(c.lead_steps + c.rounds) * 10);
    close(result.worker_memory_wait_ns, 0);
    require(result.requests == 0 && result.peak_inflight == 0, "compute-only issued memory requests");
}

void added_cost_can_hurt() {
    Config c;
    c.added_latency_ns = 10000;
    const auto w = make_workload(c);
    const auto baseline = simulate(w, c, {"base", 1, 1, false});
    const auto candidate = simulate(w, c, {"bound", 0, 0, true});
    require(candidate.execution_ns > baseline.execution_ns, "large extra overhead should lose");
    c.added_latency_ns = 0;
    const auto no_setup = simulate(w, c, {"bound", 0, 0, true});
    c.setup_ns = 1234;
    const auto setup = simulate(w, c, {"bound", 0, 0, true});
    close(setup.execution_ns - no_setup.execution_ns, 1234);
}

void validation() {
    Config c;
    c.bandwidth_gbps = 0;
    must_throw([&] { c.validate(); });
    c = Config{};
    c.coherence_ns = std::numeric_limits<double>::quiet_NaN();
    must_throw([&] { c.validate(); });
    c = Config{};
    must_throw([&] { simulate({}, c, {"bad", -1, 1, true}); });
    const Step step{0, {}};
    const Workload duplicate{{"duplicate", {{1, {step}}, {1, {step}}}}};
    must_throw([&] { simulate(duplicate, c, {"base", 1, 1, false}); });
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"serial analytical timings", serial_analytical},
        {"concurrency and memory-level parallelism", concurrency_and_mlp},
        {"phase dependencies", phase_dependencies},
        {"unchanged workload and metadata", unchanged_work_and_metadata},
        {"zero overhead gives no speedup", zero_overhead_no_speedup},
        {"metadata-only gives no speedup", metadata_only_no_speedup},
        {"compute-only timing", compute_only},
        {"additional implementation costs can hurt", added_cost_can_hurt},
        {"input validation", validation},
    };
    std::size_t failures = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
