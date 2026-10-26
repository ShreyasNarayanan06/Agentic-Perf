#include "model.hpp"
#include "coherence/region.hpp"

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace perf {
const char* name(AccessKind kind) {
    switch (kind) {
    case AccessKind::Metadata: return "metadata";
    case AccessKind::PrefixKV: return "prefix_kv";
    case AccessKind::LeafKV: return "leaf_kv";
    }
    throw std::logic_error("invalid access kind");
}
Traffic& Traffic::operator+=(const Traffic& r) {
    accesses += r.accesses; reads += r.reads; writes += r.writes;
    hits += r.hits; fills += r.fills; directory += r.directory; snoops += r.snoops;
    invalidations += r.invalidations; writebacks += r.writebacks;
    evictions += r.evictions; dirty_evictions += r.dirty_evictions;
    return *this;
}

Profile profile_workload(const Workload& workload, const Config& config, const Scenario& scenario) {
    config.validate();
    Profile profile;
    profile.workload = workload;
    std::map<std::uint64_t, std::size_t> regions;
    std::vector<coherence::RegionSpec> specs;
    std::set<std::size_t> prefixes;
    for (const auto& phase : workload)
        for (const auto& stream : phase.streams) {
            if (stream.node > config.agents) throw std::invalid_argument("node exceeds accelerator count");
            for (const auto& step : stream.steps)
                for (const auto& access : step.accesses) {
                    if (access.address % 64 != 0) throw std::invalid_argument("access must be 64B aligned");
                    const auto page = access.address / config.region_bytes;
                    auto insertion = regions.emplace(page, specs.size());
                    const auto kind = access.kind == AccessKind::Metadata ? coherence::MemoryKind::Metadata : coherence::MemoryKind::KV;
                    if (insertion.second) specs.push_back({kind, name(access.kind)});
                    else if (specs.at(insertion.first->second).kind != kind)
                        throw std::invalid_argument("metadata and KV cannot occupy the same region");
                    if (access.kind == AccessKind::PrefixKV) prefixes.insert(insertion.first->second);
                }
        }
    profile.prefix_regions = prefixes.size();
    coherence::SystemConfig system;
    system.accelerators = config.agents;
    system.region_bytes = config.region_bytes;
    coherence::Metrics metrics;
    coherence::RegionStateMachine machine(system, specs, metrics, coherence::Policy::Baseline);
    std::vector<std::list<std::uint64_t>> lru(config.agents + 1);
    const auto location = [&](std::uint64_t address) {
        return std::make_pair(regions.at(address / config.region_bytes),
                              static_cast<std::size_t>((address % config.region_bytes) / 64));
    };
    const auto valid = [&](std::size_t node, std::uint64_t address) {
        const auto pos = location(address);
        return machine.region(pos.first).lines.at(pos.second).copies.at(node).valid;
    };
    const auto touch = [&](std::size_t node, Access& access) {
        auto& order = lru.at(node);
        for (auto it = order.begin(); it != order.end();) {
            if (!valid(node, *it)) it = order.erase(it);
            else ++it;
        }
        const auto before = metrics;
        Service service;
        auto& t = service.traffic;
        const auto pos = location(access.address);
        const auto existing = std::find(order.begin(), order.end(), access.address);
        if (existing != order.end()) order.erase(existing);
        else if (config.cache_bytes != 0 && order.size() == config.cache_bytes / 64) {
            const auto victim = location(order.back());
            ++t.evictions;
            if (machine.evict(static_cast<int>(node), victim.first, victim.second)) ++t.dirty_evictions;
            order.pop_back();
        }
        const auto& line = machine.region(pos.first).lines.at(pos.second);
        service.peer_fill = line.exclusive_owner != coherence::no_owner &&
            line.exclusive_owner != static_cast<int>(node);
        coherence::Request request;
        request.node = static_cast<int>(node);
        request.region = pos.first;
        request.line = pos.second;
        request.operation = access.write ? coherence::Operation::Write : coherence::Operation::Read;
        machine.access(request);
        if (config.cache_bytes == 0) {
            ++t.evictions;
            if (machine.evict(static_cast<int>(node), pos.first, pos.second)) ++t.dirty_evictions;
        } else order.push_front(access.address);
        t.accesses = 1; t.reads = access.write ? 0 : 1; t.writes = access.write ? 1 : 0;
        t.hits = metrics.cache_hits - before.cache_hits;
        t.fills = metrics.data_fills - before.data_fills;
        t.directory = metrics.directory_lookups - before.directory_lookups;
        t.snoops = metrics.snoops - before.snoops;
        t.invalidations = metrics.invalidations - before.invalidations;
        t.writebacks = metrics.writebacks - before.writebacks;
        access.service = service;
        profile.kinds[index(access.kind)] += t;
    };

    // Fixed logical interleaving for profiling, independent of timing changes:
    // step 0 on all streams, then step 1, with round-robin accesses in each step.
    for (auto& phase : profile.workload) {
        if (phase.fork_boundary) {
            for (const auto id : prefixes)
                for (const auto& line : machine.region(id).lines)
                    for (const auto& copy : line.copies)
                        if (copy.valid && copy.dirty) ++profile.fork_dirty_lines;
            if (scenario.counterfactual && config.flush_at_fork) {
                const auto before = metrics.writebacks;
                for (const auto id : prefixes) machine.writeback_region(id);
                profile.fork_flushes += metrics.writebacks - before;
            }
        }
        std::size_t steps = 0;
        for (const auto& stream : phase.streams) steps = std::max(steps, stream.steps.size());
        for (std::size_t s = 0; s < steps; ++s) {
            std::size_t accesses = 0;
            for (const auto& stream : phase.streams)
                if (s < stream.steps.size()) accesses = std::max(accesses, stream.steps[s].accesses.size());
            for (std::size_t a = 0; a < accesses; ++a)
                for (auto& stream : phase.streams)
                    if (s < stream.steps.size() && a < stream.steps[s].accesses.size())
                        touch(stream.node, stream.steps[s].accesses[a]);
        }
    }
    return profile;
}
} // namespace perf
