#include "simulation/FlowRecord.hpp"

#include <array>
#include <utility>

namespace spacetrains::simulation {

namespace {
constexpr std::array<const char*, kFlowKinds> kKindIds {"delivered", "loaded", "passengers", "lane"};
}  // namespace

const char* flow_kind_id(FlowKind kind) {
    return kKindIds.at(static_cast<std::size_t>(kind));
}

bool flow_kind_from_id(const std::string& id, FlowKind& out) {
    for (std::size_t k = 0; k < kKindIds.size(); ++k) {
        if (id == kKindIds[k]) {
            out = static_cast<FlowKind>(k);
            return true;
        }
    }
    return false;
}

void FlowRecord::reset(std::vector<std::string> stations, std::vector<std::string> goods,
    std::vector<std::vector<double>> start_stocks) {
    stations_ = std::move(stations);
    goods_ = std::move(goods);
    start_stocks_ = std::move(start_stocks);
    lifetime_.clear();
    period_.clear();
    history_.clear();
}

void FlowRecord::record(FlowKind kind, const std::string& from, const std::string& to, const std::string& item, double units,
    double value, double count) {
    if (units <= 0.0 && value <= 0.0) {
        return;
    }
    const FlowKey key {.kind = kind, .from = from, .to = to, .item = item};
    for (auto* table : {&lifetime_, &period_}) {
        auto& amount = (*table)[key];
        amount.units += units;
        amount.value += value;
        amount.count += count;
    }
}

void FlowRecord::close_period(double day, std::vector<std::vector<double>> stocks) {
    history_.push_back({.day = day, .flows = std::move(period_), .stocks = std::move(stocks)});
    period_.clear();
}

}  // namespace spacetrains::simulation
