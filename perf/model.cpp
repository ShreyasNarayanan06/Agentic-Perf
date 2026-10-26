#include "model.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace perf {
void Config::validate() const {
    if (agents == 0 || agents > 63 || lead_steps == 0 || rounds == 0 || outstanding == 0 ||
        prefix_lines == 0 || leaf_lines == 0 || stores_per_line == 0)
        throw std::invalid_argument("agents must be 1..63; steps, windows and working sets must be positive");
    if (cache_bytes % 64 != 0 || region_bytes == 0 || region_bytes % 64 != 0)
        throw std::invalid_argument("cache/region bytes must be multiples of 64; region size must be positive");
    if (prefix_lines > (1U << 24) || leaf_lines > (1U << 24))
        throw std::invalid_argument("working set exceeds supported address range");
    for (const auto value : {compute_ns, host_compute_ns, hit_ns, memory_ns, peer_ns, directory_ns,
            snoop_ns, invalidate_ns, writeback_ns, directory_bytes, snoop_bytes, invalidate_bytes,
            fork_barrier_ns, region_barrier_ns, setup_ns, recovery_ns, recovery_bytes, added_latency_ns})
        if (!std::isfinite(value) || value < 0) throw std::invalid_argument("costs must be finite and nonnegative");
    for (const auto value : {overlap_trigger, recovery_rate})
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("fractions must be in [0, 1]");
    if (!std::isfinite(bandwidth_gbps) || bandwidth_gbps <= 0)
        throw std::invalid_argument("bandwidth must be finite and positive");
}

namespace {
Step make_step(const Config& c, std::size_t round, std::size_t node, bool lead) {
    Step result;
    result.compute_ns = node == 0 ? c.host_compute_ns : c.compute_ns;
    if (node != 0 && !lead)
        for (std::size_t r = 0; r < c.prefix_reads; ++r)
            result.accesses.push_back({AccessKind::PrefixKV, ((round * c.prefix_reads + r) % c.prefix_lines) * 64, false, {}});
    if (node != 0)
        for (std::size_t w = 0; w < c.leaf_writes; ++w) {
            const auto line = (round * c.leaf_writes + w) / c.stores_per_line;
            const auto address = lead ? (line % c.prefix_lines) * 64 :
                (1ULL << 40) + node * (1ULL << 32) + (line % c.leaf_lines) * 64;
            result.accesses.push_back({lead ? AccessKind::PrefixKV : AccessKind::LeafKV, address, true, {}});
        }
    for (std::size_t m = 0; m < c.metadata_ops; ++m)
        result.accesses.push_back({AccessKind::Metadata, (1ULL << 60) + (m % 8) * 64, m % 2 == 0, {}});
    return result;
}
enum class EventKind { ComputeDone, StartBatch, Completion };
struct Event {
    double time;
    std::uint64_t order;
    std::size_t stream, step;
    EventKind kind;
    bool operator>(const Event& r) const { return std::tie(time, order) > std::tie(r.time, r.order); }
};
struct StepState {
    bool ready = false, started = false, next_compute = false;
    std::size_t issued = 0, completed = 0, inflight = 0;
    double wait_start = 0, next_compute_start = 0, next_compute_end = 0;
};
} // namespace

Workload make_workload(const Config& c) {
    c.validate();
    Workload w;
    Phase lead{"lead", {{1, {}}}, false};
    for (std::size_t r = 0; r < c.lead_steps; ++r) lead.streams.front().steps.push_back(make_step(c, r, 1, true));
    w.push_back(std::move(lead));
    Phase children{"parallel_children", {}, true};
    for (std::size_t node = 0; node <= c.agents; ++node) {
        Stream stream{node, {}};
        for (std::size_t r = 0; r < c.rounds; ++r) stream.steps.push_back(make_step(c, r, node, false));
        children.streams.push_back(std::move(stream));
    }
    w.push_back(std::move(children));
    Phase complete{"completion", {{0, {}}}, false};
    for (std::size_t n = 0; n <= c.agents; ++n) complete.streams.front().steps.push_back(make_step(c, 0, 0, false));
    w.push_back(std::move(complete));
    return w;
}
std::vector<Scenario> default_scenarios() {
    return {{"Reference overhead", 1, 1, false}, {"25% less KV overhead", .75, .75, true},
            {"50% less KV overhead", .5, .5, true}, {"75% less KV overhead", .25, .25, true},
            {"Zero KV event overhead", 0, 0, true}, {"50% less KV latency only", .5, 1, true},
            {"50% less KV traffic only", 1, .5, true}};
}

Result simulate(const Workload& workload, const Config& c, const Scenario& scenario) {
    c.validate();
    for (const auto fraction : {scenario.latency_retained, scenario.traffic_retained})
        if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
            throw std::invalid_argument("retained fractions must be in [0, 1]");
    const auto profile = profile_workload(workload, c, scenario);
    Result result;
    result.scenario = scenario;
    result.kinds = profile.kinds;
    result.fork_flushes = profile.fork_flushes;
    result.prefix_regions = profile.prefix_regions;
    double link_available = 0;
    std::size_t inflight = 0;
    std::uint64_t order = 0;
    std::vector<std::unordered_map<std::uint64_t, double>> line_ready(c.agents + 1);
    const auto network = [&](double issue, double bytes) {
        if (bytes == 0) return issue;
        const auto start = std::max(issue, link_available);
        const auto duration = bytes / c.bandwidth_gbps;
        result.queue_wait_ns += start - issue;
        result.link_busy_ns += duration;
        link_available = start + duration;
        return link_available;
    };
    bool fork_processed = false;
    for (const auto& phase : profile.workload) {
        if (phase.fork_boundary && !fork_processed) {
            fork_processed = true;
            const auto begin = result.execution_ns;
            if (scenario.counterfactual && profile.prefix_regions != 0) {
                result.execution_ns += c.setup_ns + c.fork_barrier_ns +
                    static_cast<double>(profile.prefix_regions) * c.region_barrier_ns;
                const auto controls = c.directory_bytes * static_cast<double>(c.agents + 1 + profile.prefix_regions);
                result.control_bytes += controls;
                result.execution_ns = network(result.execution_ns, controls);
                std::priority_queue<double, std::vector<double>, std::greater<double>> pending;
                for (std::uint64_t n = 0; n < profile.fork_flushes; ++n) {
                    auto issue = result.execution_ns;
                    if (pending.size() == c.outstanding) { issue = pending.top(); pending.pop(); }
                    const auto done = network(issue, 64) + c.memory_ns + c.writeback_ns;
                    pending.push(done);
                    result.writeback_bytes += 64;
                }
                while (!pending.empty()) {
                    result.execution_ns = std::max(result.execution_ns, pending.top()); pending.pop();
                }
            }
            result.fork_penalty_ns = result.execution_ns - begin;
            result.phases.push_back({"fork_overhead", begin, result.execution_ns});
        }
        const auto phase_start = result.execution_ns;
        auto phase_end = phase_start;
        std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events;
        std::vector<std::vector<StepState>> states(phase.streams.size());
        std::vector<std::size_t> current(phase.streams.size(), 0);
        std::set<std::size_t> nodes;
        for (std::size_t s = 0; s < phase.streams.size(); ++s) {
            const auto& stream = phase.streams[s];
            if (!nodes.insert(stream.node).second) throw std::invalid_argument("one stream per node is required");
            for (const auto& step : stream.steps)
                if (!std::isfinite(step.compute_ns) || step.compute_ns < 0) throw std::invalid_argument("invalid compute duration");
            states[s].resize(stream.steps.size());
            if (stream.steps.empty()) continue;
            result.worker_compute_ns += stream.steps.front().compute_ns;
            events.push({phase_start + stream.steps.front().compute_ns, order++, s, 0, EventKind::ComputeDone});
        }
        while (!events.empty()) {
            const auto event = events.top(); events.pop();
            phase_end = std::max(phase_end, event.time);
            const auto& stream = phase.streams[event.stream];
            const auto& step = stream.steps.at(event.step);
            auto& state = states[event.stream].at(event.step);
            if (event.kind == EventKind::ComputeDone) state.ready = true;
            if (event.step != current[event.stream]) continue; // bounded one-step lookahead
            if (!state.started) {
                if (!state.ready) throw std::logic_error("memory batch started before compute");
                state.started = true; state.wait_start = event.time;
            }
            if (event.kind == EventKind::Completion) {
                --state.inflight; --inflight; ++state.completed;
            }
            const auto trigger = stream.node == 0 ? 1.0 : c.overlap_trigger;
            const auto threshold = static_cast<std::size_t>(std::ceil(static_cast<double>(step.accesses.size()) * trigger));
            if (!state.next_compute && state.completed >= threshold && event.step + 1 < stream.steps.size()) {
                state.next_compute = true;
                state.next_compute_start = event.time;
                const auto compute = stream.steps[event.step + 1].compute_ns;
                state.next_compute_end = event.time + compute;
                result.worker_compute_ns += compute;
                events.push({state.next_compute_end, order++, event.stream, event.step + 1, EventKind::ComputeDone});
            }
            while (state.issued < step.accesses.size() && state.inflight < c.outstanding) {
                const auto& access = step.accesses[state.issued++];
                const auto& t = access.service.traffic;
                const bool kv = access.kind != AccessKind::Metadata;
                const auto fraction_ns = kv ? scenario.latency_retained : 1;
                const auto fraction_bytes = kv ? scenario.traffic_retained : 1;
                const auto coherence = static_cast<double>(t.directory) * c.directory_ns +
                    static_cast<double>(t.snoops) * c.snoop_ns + static_cast<double>(t.invalidations) * c.invalidate_ns;
                const auto controls = static_cast<double>(t.directory) * c.directory_bytes +
                    static_cast<double>(t.snoops) * c.snoop_bytes + static_cast<double>(t.invalidations) * c.invalidate_bytes;
                const auto recoveries = kv && scenario.counterfactual && t.directory != 0 ? c.recovery_rate : 0;
                const auto extra = kv && scenario.counterfactual ? c.added_latency_ns : 0;
                const auto control = controls * fraction_bytes + static_cast<double>(t.evictions) * 8 + recoveries * c.recovery_bytes;
                const auto payload = static_cast<double>(t.fills + t.writebacks) * 64;
                const auto earliest = std::max(event.time, line_ready[stream.node][access.address]);
                const auto base = t.fills != 0 ? (access.service.peer_fill ? c.peer_ns : c.memory_ns) : c.hit_ns;
                const auto done = network(earliest, payload + control) + base + coherence * fraction_ns +
                    static_cast<double>(t.writebacks) * c.writeback_ns + static_cast<double>(t.evictions) * 4 +
                    recoveries * c.recovery_ns + extra;
                line_ready[stream.node][access.address] = done;
                events.push({done, order++, event.stream, event.step, EventKind::Completion});
                result.data_bytes += static_cast<double>(t.fills) * 64;
                result.writeback_bytes += static_cast<double>(t.writebacks) * 64;
                result.control_bytes += control;
                result.assumed_coherence_latency_ns += coherence * fraction_ns;
                result.added_latency_ns += recoveries * c.recovery_ns + extra;
                result.expected_recoveries += recoveries;
                ++result.requests;
                if (payload + control != 0) ++result.offchip_requests;
                ++state.inflight; ++inflight;
                result.peak_inflight = std::max(result.peak_inflight, inflight);
            }
            if (state.completed == step.accesses.size()) {
                result.worker_memory_wait_ns += event.time - state.wait_start;
                if (state.next_compute)
                    result.overlap_ns += std::max(0.0, std::min(event.time, state.next_compute_end) - state.next_compute_start);
                ++current[event.stream];
                if (current[event.stream] < stream.steps.size() && states[event.stream][current[event.stream]].ready)
                    events.push({event.time, order++, event.stream, current[event.stream], EventKind::StartBatch});
            }
        }
        result.phases.push_back({phase.label, phase_start, phase_end});
        result.execution_ns = phase_end;
    }
    return result;
}
} // namespace perf
