// mdr_gen: write a synthetic session (line_a.bin, line_b.bin, clean.bin,
// snapshots.bin, truth.txt) into --out DIR.
#include "cli.hpp"
#include "mdr/files.hpp"
#include "mdr/generator.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
  try {
    const mdr::Args a(argc, argv);
    mdr::GenParams p;
    const auto dir = a.str("out", "build/data");
    p.messages = a.u64("messages", p.messages);
    p.instruments = static_cast<std::uint16_t>(a.u64("instruments", p.instruments));
    p.seed = a.u64("seed", p.seed);
    p.loss_a = a.f64("loss-a", p.loss_a);
    p.loss_b = a.f64("loss-b", p.loss_b);
    p.dup = a.f64("dup", p.dup);
    p.reorder = a.f64("reorder", p.reorder);
    p.max_per_packet = static_cast<std::uint32_t>(a.u64("max-per-packet", p.max_per_packet));
    p.snapshot_every = a.u64("snapshot-every", p.snapshot_every);
    p.outage_every = a.u64("outage-every", p.outage_every);
    p.outage_len = static_cast<std::uint32_t>(a.u64("outage-len", p.outage_len));
    p.target_orders = static_cast<std::uint32_t>(a.u64("target-orders", p.target_orders));
    p.corrupt_snapshot = static_cast<int>(a.f64("corrupt-snapshot", -1));
    a.reject_unknown();

    const auto t0 = std::chrono::steady_clock::now();
    const auto g = mdr::generate(p);
    std::filesystem::create_directories(dir);
    mdr::write_file(dir + "/line_a.bin", g.line_a);
    mdr::write_file(dir + "/line_b.bin", g.line_b);
    mdr::write_file(dir + "/clean.bin", g.clean);
    mdr::write_file(dir + "/snapshots.bin", g.snapshots);
    char digest[32];
    std::snprintf(digest, sizeof digest, "%016llx", static_cast<unsigned long long>(g.truth_digest));
    const auto& s = g.stats;
    std::map<std::string, std::string> kv{
        {"messages", std::to_string(g.messages)},
        {"instruments", std::to_string(p.instruments)},
        {"seed", std::to_string(p.seed)},
        {"digest", digest},
        {"packets", std::to_string(s.packets)},
        {"lost_a", std::to_string(s.lost_a)},
        {"lost_b", std::to_string(s.lost_b)},
        {"lost_both", std::to_string(s.lost_both)},
        {"dup_a", std::to_string(s.dup_a)},
        {"dup_b", std::to_string(s.dup_b)},
        {"late_a", std::to_string(s.late_a)},
        {"late_b", std::to_string(s.late_b)},
        {"snapshots", std::to_string(s.snapshots)},
        {"adds", std::to_string(s.adds)},
        {"cancels", std::to_string(s.cancels)},
        {"deletes", std::to_string(s.deletes)},
        {"executes", std::to_string(s.executes)},
        {"modifies", std::to_string(s.modifies)},
        {"avg_live_orders", std::to_string(s.avg_live_orders)},
        {"avg_levels_per_side", std::to_string(s.avg_levels_per_side)},
        {"max_levels_per_side", std::to_string(s.max_levels_per_side)},
    };
    mdr::write_kv(dir + "/truth.txt", kv);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "wrote " << dir << " in " << secs << " s\n";
    for (const auto& [k, v] : kv) std::cout << "  " << k << '=' << v << '\n';
    std::cout << "  line_a_bytes=" << g.line_a.size() << " line_b_bytes=" << g.line_b.size()
              << " snapshot_bytes=" << g.snapshots.size() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
