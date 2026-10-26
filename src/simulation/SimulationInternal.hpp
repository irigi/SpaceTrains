#pragma once

// Constants and small helpers shared by the files that implement Simulation
// (Simulation.cpp and the topic files next to it). Not part of the public interface.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "domain/Types.hpp"
#include "economy/EconomySystem.hpp"
#include "math/Vec3d.hpp"

namespace spacetrains::simulation::detail {

inline constexpr double FUEL_UNITS_TO_KG = 100.0;
// Propellant a plan uses up: burned, plus hydrogen boiled off in flight (bought, never burned).
inline double consumed_propellant_kg(const domain::TrajectoryPlan& plan) {
    return plan.propellant_required_kg + plan.propellant_boiled_kg;
}
inline constexpr std::size_t MAX_EVENT_HISTORY = 24;
inline constexpr std::size_t MAX_TRADE_HISTORY = 24;
// A cargo run must clear this profit rate or the ship repositions toward demand
// instead. Without a floor, ships accept months-long near-zero-profit hauls that
// tie up the fleet (observed: 1u water, 14 cr, 183 d transit). Long interplanetary
// runs legitimately score ~15-25 cr/day, so the floor must sit well below that.
inline constexpr double MIN_MISSION_SCORE_PER_DAY = 2.0;
// Provisions are bought for the whole mission (wait + transit) plus this margin,
// and enough for a return leg (at least this many days): destinations are often
// starving stations that cannot spare food or oxygen, and a crew stuck there could
// never leave.
inline constexpr double PROVISION_MARGIN = 1.1;
inline constexpr double PROVISION_MIN_RETURN_DAYS = 120.0;
// Docked crews top up provisions from the local market when below the first
// threshold, buying enough for the second.
inline constexpr double DOCKED_PROVISION_LOW_DAYS = 7.0;
inline constexpr double DOCKED_PROVISION_TARGET_DAYS = 14.0;
// Crews keep two years of life support aboard where ports can spare it: outer-system
// legs take 400-1100 days and the outer stations themselves import their food.
inline constexpr double PROVISION_ENDURANCE_DAYS = 730.0;
// An idle crewed ship with nothing profitable to fly for this long lays up,
// whatever its balance: no owner pays an idle crew for months.
inline constexpr double IDLE_LAYUP_DAYS = 30.0;
// A laid-up ship has no crew; its owner looks for work for it every few days.
inline constexpr double LAYUP_REVIEW_DAYS = 5.0;
inline constexpr double IDLE_REVIEW_S = 6.0 * 3600.0;
// Ships load the propellant a mission burns plus this reserve, not a full tank.
inline constexpr double PROPELLANT_RESERVE_FRACTION = 0.1;
// Emergency deliveries (step 14): opened when a life-support good's forecast stock runs out
// within the horizon, closed when it covers EMERGENCY_CLOSE_DAYS again. The premium price covers
// one delivery of EMERGENCY_COVER_DAYS of use (shared by the ships that take it), paid by the
// faction above the order's price. It starts at the station's own cap (step 29) and grows by
// EMERGENCY_RAISE_FACTOR every EMERGENCY_RAISE_DAYS that no ship takes a delivery, up to
// orders.life_value: a small station's month of water is worth little at 3x, less than a trip to
// it costs, so the offer grows until a ship comes (Mercury's water stayed short for two years
// at 8x; doubling to 96x paid 2M cr in two years).
// A station's import cost (what its residents pay) averages deliveries over this many days of its use.
inline constexpr double IMPORT_COST_DAYS = 30.0;
// A faction covers a station in the red within about this many days (economy overhaul).
inline constexpr double INSOLVENCY_COVER_DAYS = 2.0;
inline constexpr double EMERGENCY_HORIZON_DAYS = 21.0;
inline constexpr double EMERGENCY_CLOSE_DAYS = 14.0;
inline constexpr double EMERGENCY_COVER_DAYS = 30.0;
inline constexpr double EMERGENCY_RAISE_FACTOR = 1.5;
inline constexpr double EMERGENCY_RAISE_DAYS = 15.0;

// Tank refits (part C): a docked ship at its home base compares the next smaller and next
// larger tank variants this often (each comparison is a full dispatch pass, ~0.3 s), and
// refits when the better missions repay the yard within the payback period.
inline constexpr double REFIT_REVIEW_DAYS = 90.0;
inline constexpr double REFIT_PAYBACK_DAYS = 180.0;
// Ships carry their return fuel to ports that sell it at more than this multiple of the
// departure port's price.
inline constexpr double TANKERING_PRICE_RATIO = 2.0;
// Ships' fuel purchases and cargo trades at a station are averaged over about this many days.
inline constexpr double TRADE_FLOW_DAYS = 60.0;

inline math::Vec3d interpolate_sampled_path(const std::vector<math::Vec3d>& path, double progress) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1) {
        return path.front();
    }

    const double clamped_progress = std::clamp(progress, 0.0, 1.0);
    const double scaled_index = clamped_progress * static_cast<double>(path.size() - 1);
    const auto lower_index = static_cast<std::size_t>(std::floor(scaled_index));
    const auto upper_index = std::min(lower_index + 1, path.size() - 1);
    const double segment_alpha = scaled_index - static_cast<double>(lower_index);
    return path[lower_index] * (1.0 - segment_alpha) + path[upper_index] * segment_alpha;
}

inline double interpolate_timed_scalar(
    const std::vector<double>& values,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (values.empty()) {
        return 0.0;
    }
    if (values.size() == 1 || sample_times_s.size() != values.size()) {
        return values.front();
    }
    if (time_s <= sample_times_s.front()) {
        return values.front();
    }
    if (time_s >= sample_times_s.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return values[lower_index] * (1.0 - alpha) + values[upper_index] * alpha;
}

inline math::Vec3d interpolate_timed_sampled_path(
    const std::vector<math::Vec3d>& path,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1 || sample_times_s.size() != path.size()) {
        return path.front();
    }
    if (time_s <= sample_times_s.front()) {
        return path.front();
    }
    if (time_s >= sample_times_s.back()) {
        return path.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

inline std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

// The largest lot's good (the UI's one-line summary of a hold).
inline std::string largest_lot(const std::vector<domain::CargoLot>& cargo) {
    std::string largest;
    double units = 0.0;
    for (const auto& lot : cargo) {
        if (lot.units > units) {
            units = lot.units;
            largest = lot.commodity_id;
        }
    }
    return largest;
}

inline std::string cargo_json(const std::vector<domain::CargoLot>& cargo) {
    std::string text = "[";
    for (const auto& lot : cargo) {
        text += std::format("{}{{\"commodity_id\":\"{}\",\"units\":{:.6f},\"contract_value\":{:.2f}}}", text.size() > 1 ? "," : "",
            json_escape(lot.commodity_id), lot.units, lot.contract_value);
    }
    return text + "]";
}

// "30u food + 20u water" (the largest lot first).
inline std::string describe_cargo(std::vector<domain::CargoLot> cargo) {
    std::sort(cargo.begin(), cargo.end(), [](const auto& a, const auto& b) { return a.units > b.units; });
    std::string text;
    for (const auto& lot : cargo) {
        text += std::format("{}{:.0f}u {}", text.empty() ? "" : " + ", lot.units, lot.commodity_id);
    }
    return text;
}


// Units a station will sell for provisions: consumers keep 14 days of their own
// use, everyone else a small working stock.
inline double sellable_reserve_units(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const std::string& commodity_id,
    bool departing) {
    const auto& rates = economy.get_station_net_rates(station_def);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    return departing ? 0.0 : (rate < 0.0 ? std::abs(rate) * 14.0 : 5.0);
}

inline double sellable_units(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const domain::StationState& station_state,
    const std::string& commodity_id,
    bool departing) {
    const auto it = station_state.inventory.find(commodity_id);
    const double stock = it == station_state.inventory.end() ? 0.0 : it->second;
    return std::max(0.0, stock - sellable_reserve_units(economy, station_def, commodity_id, departing));
}

// Fuel a station can sell to a ship arriving in `days`, counting the depot's refills
// meanwhile (but not other ships' purchases).
inline double fuel_for_sale_on_arrival_kg(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const domain::StationState& station_state,
    double days) {
    const auto it = station_state.inventory.find(economy::FUEL_ID);
    const double stock = it == station_state.inventory.end() ? 0.0 : it->second;
    const double reserve = sellable_reserve_units(economy, station_def, economy::FUEL_ID, false);
    return std::max(0.0, economy.fuel_stock_after_days(station_def, stock, days) - reserve) * FUEL_UNITS_TO_KG;
}

}  // namespace spacetrains::simulation::detail
