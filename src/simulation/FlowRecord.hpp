// What moved where (docs/modules/economy.md, "Flow record"): goods delivered and loaded, surface
// trade, passengers and the cost of each lane, from place to place, summed since the start and per
// month like the money journal. It only records: nothing in the simulation reads it back, so it
// cannot change the game (the statistics screen's Flows tab shows it).
#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace spacetrains::simulation {

enum class FlowKind : std::uint8_t {
    Delivered,   // goods arriving: ship cargo sold at a station, or surface goods sold to a station
    Loaded,      // ship cargo bought at its origin (destination: where it is going)
    Passengers,  // people landed; item: settlers, emigrants, rotation, visitors
    Lane,        // a ship's arrival: units = cargo unit-days, value = propellant (kg), count = trips
};
inline constexpr int kFlowKinds = 4;

[[nodiscard]] const char* flow_kind_id(FlowKind kind);
[[nodiscard]] bool flow_kind_from_id(const std::string& id, FlowKind& out);

// Places are station ids, or "surface:<economy id>" for Earth and the colonies below their gateways.
struct FlowKey {
    FlowKind kind {FlowKind::Delivered};
    std::string from;
    std::string to;
    std::string item;   // a commodity id, a passenger group, or empty (lanes)
    auto operator<=>(const FlowKey&) const = default;
};

struct FlowAmount {
    double units {0.0};
    double value {0.0};   // credits paid (goods, fares); propellant kg (lanes)
    double count {0.0};   // deliveries, landings or trips
};

using FlowTable = std::map<FlowKey, FlowAmount>;

// One closed month: what moved during it, and every station's stock at its end (in
// stations_order, goods in goods_order), for the goods balance.
struct FlowSample {
    double day {0.0};
    FlowTable flows;
    std::vector<std::vector<double>> stocks;
};

class FlowRecord {
public:
    // The stations and goods whose stocks the samples keep, and their stocks at the start.
    void reset(std::vector<std::string> stations, std::vector<std::string> goods, std::vector<std::vector<double>> start_stocks);
    void record(FlowKind kind, const std::string& from, const std::string& to, const std::string& item, double units,
        double value, double count = 1.0);
    // Close the running month at `day` with the stations' stocks then.
    void close_period(double day, std::vector<std::vector<double>> stocks);

    [[nodiscard]] const std::vector<std::string>& stations() const { return stations_; }
    [[nodiscard]] const std::vector<std::string>& goods() const { return goods_; }
    [[nodiscard]] const std::vector<std::vector<double>>& start_stocks() const { return start_stocks_; }
    [[nodiscard]] const FlowTable& lifetime() const { return lifetime_; }
    [[nodiscard]] const FlowTable& period() const { return period_; }
    [[nodiscard]] const std::vector<FlowSample>& history() const { return history_; }

    // Persistence (SimulationPersistence.cpp) restores these directly.
    std::vector<std::vector<double>>& start_stocks_mutable() { return start_stocks_; }
    FlowTable& lifetime_mutable() { return lifetime_; }
    FlowTable& period_mutable() { return period_; }
    std::vector<FlowSample>& history_mutable() { return history_; }

private:
    std::vector<std::string> stations_;
    std::vector<std::string> goods_;
    std::vector<std::vector<double>> start_stocks_;
    FlowTable lifetime_;
    FlowTable period_;
    std::vector<FlowSample> history_;
};

}  // namespace spacetrains::simulation
