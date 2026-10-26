#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace perf {

void Config::validate() const {
    if (agents == 0 || agents > 256 || lead_steps == 0 || rounds == 0 || outstanding == 0)
        throw std::invalid_argument("agents must be 1..256; steps, rounds and outstanding must be positive");
    for (const auto value : {compute_ns, host_compute_ns, memory_ns, coherence_ns,
                            coherence_bytes, added_latency_ns, setup_ns})
        if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("latencies, compute and control bytes must be finite and nonnegative");
    if (!std::isfinite(bandwidth_gbps) || bandwidth_gbps <= 0)
        throw std::invalid_argument("bandwidth must be finite and positive");
}

namespace {

Step step(const Config& config, std::size_t round, std::size_t node, bool lead) {
    Step result;
    result.compute_ns = node == 0 ? config.host_compute_ns : config.compute_ns;
    if (node != 0 && !lead)
        for (std::size_t r = 0; r < config.prefix_reads; ++r)
            result.accesses.push_back({AccessKind::PrefixKV, (r % 64) * 64, false});
    if (node != 0)
        for (std::size_t w = 0; w < config.leaf_writes; ++w) {
            const auto address = lead ? ((round * config.leaf_writes + w) % 64) * 64
                : 0x100000ULL + node * 0x10000ULL + ((round * config.leaf_writes + w) % 64) * 64;
            result.accesses.push_back({lead ? AccessKind::PrefixKV : AccessKind::LeafKV, address, true});
        }
    for (std::size_t m = 0; m < config.metadata_ops; ++m)
        result.accesses.push_back({AccessKind::Metadata, 0x100000000ULL + (m % 8) * 64, m % 2 == 0});
    return result;
}

enum class EventKind { StepReady, Completion };

struct Event {
    double time;
    std::uint64_t order;
    std::size_t stream;
    EventKind kind;
    bool operator>(const Event& other) const {
        return std::tie(time, order) > std::tie(other.time, other.order);
    }
};

struct Cursor {
    std::size_t step = 0;
    std::size_t next_access = 0;
    std::size_t inflight = 0;
    std::size_t completed = 0;
    double wait_start = 0;
};

} // namespace

Workload make_workload(const Config& config) {
    config.validate();
    Workload workload;
    Phase lead{"lead", {Stream{1, {}}}};
    for (std::size_t r = 0; r < config.lead_steps; ++r)
        lead.streams.front().steps.push_back(step(config, r, 1, true));
    workload.push_back(std::move(lead));
    Phase children{"parallel_children", {}};
    for (std::size_t node = 0; node <= config.agents; ++node) {
        Stream stream{node, {}};
        for (std::size_t r = 0; r < config.rounds; ++r)
            stream.steps.push_back(step(config, r, node, false));
        children.streams.push_back(std::move(stream));
    }
    workload.push_back(std::move(children));
    Phase complete{"completion", {Stream{0, {}}}};
    for (std::size_t node = 0; node <= config.agents; ++node)
        complete.streams.front().steps.push_back(step(config, 0, 0, false));
    workload.push_back(std::move(complete));
    return workload;
}

std::vector<Scenario> default_scenarios() {
    return {{"Reference overhead", 1, 1, false},
            {"25% less KV overhead", .75, .75, true},
            {"50% less KV overhead", .5, .5, true},
            {"75% less KV overhead", .25, .25, true},
            {"Zero KV overhead", 0, 0, true},
            {"50% less KV latency only", .5, 1, true},
            {"50% less KV traffic only", 1, .5, true}};
}

Result simulate(const Workload& workload, const Config& config, const Scenario& scenario) {
    config.validate();
    for (const auto fraction : {scenario.latency_retained, scenario.traffic_retained})
        if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
            throw std::invalid_argument("retained overhead fractions must be in [0, 1]");
    Result result;
    result.scenario = scenario;
    result.execution_ns = scenario.counterfactual ? config.setup_ns : 0;
    double link_available = 0;
    std::size_t global_inflight = 0;
    std::uint64_t order = 0;

    for (const auto& phase : workload) {
        const auto phase_start = result.execution_ns;
        double phase_end = phase_start;
        std::vector<Cursor> cursors(phase.streams.size());
        std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events;
        std::set<std::size_t> nodes;
        for (std::size_t s = 0; s < phase.streams.size(); ++s) {
            const auto& stream = phase.streams[s];
            if (!nodes.insert(stream.node).second)
                throw std::invalid_argument("only one stream per node is supported within a phase");
            for (const auto& current : stream.steps)
                if (!std::isfinite(current.compute_ns) || current.compute_ns < 0)
                    throw std::invalid_argument("invalid step compute time");
            if (stream.steps.empty()) continue;
            result.worker_compute_ns += stream.steps.front().compute_ns;
            events.push({phase_start + stream.steps.front().compute_ns, order++, s, EventKind::StepReady});
        }
        while (!events.empty()) {
            const auto event = events.top();
            events.pop();
            phase_end = std::max(phase_end, event.time);
            auto& cursor = cursors[event.stream];
            const auto& stream = phase.streams[event.stream];
            const auto& current = stream.steps.at(cursor.step);
            if (event.kind == EventKind::StepReady) {
                cursor.next_access = 0;
                cursor.completed = 0;
                cursor.wait_start = event.time;
            } else {
                --cursor.inflight;
                --global_inflight;
                ++cursor.completed;
            }

            while (cursor.next_access < current.accesses.size() && cursor.inflight < config.outstanding) {
                const auto& access = current.accesses[cursor.next_access++];
                const bool eligible = access.kind != AccessKind::Metadata;
                const auto retained_latency = eligible ? scenario.latency_retained : 1;
                const auto retained_traffic = eligible ? scenario.traffic_retained : 1;
                const auto extra_latency = eligible && scenario.counterfactual ? config.added_latency_ns : 0;
                const auto control = config.coherence_bytes * retained_traffic;
                const auto start = std::max(event.time, link_available);
                const auto serialization = (64 + control) / config.bandwidth_gbps;
                link_available = start + serialization;
                const auto completion = link_available + config.memory_ns +
                    config.coherence_ns * retained_latency + extra_latency;
                events.push({completion, order++, event.stream, EventKind::Completion});
                result.queue_wait_ns += start - event.time;
                result.link_busy_ns += serialization;
                result.data_bytes += 64;
                result.control_bytes += control;
                result.assumed_coherence_latency_ns += config.coherence_ns * retained_latency;
                result.added_latency_ns += extra_latency;
                ++result.requests;
                if (access.kind == AccessKind::Metadata) ++result.metadata_requests;
                else if (access.kind == AccessKind::PrefixKV) ++result.prefix_requests;
                else ++result.leaf_requests;
                ++cursor.inflight;
                ++global_inflight;
                result.peak_inflight = std::max(result.peak_inflight, global_inflight);
            }

            if (cursor.completed == current.accesses.size()) {
                result.worker_memory_wait_ns += event.time - cursor.wait_start;
                ++cursor.step;
                if (cursor.step < stream.steps.size()) {
                    const auto compute = stream.steps[cursor.step].compute_ns;
                    result.worker_compute_ns += compute;
                    events.push({event.time + compute, order++, event.stream, EventKind::StepReady});
                }
            }
        }
        result.phases.push_back({phase.label, phase_start, phase_end});
        result.execution_ns = phase_end;
    }
    return result;
}

} // namespace perf
