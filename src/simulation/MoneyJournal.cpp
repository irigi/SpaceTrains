#include "simulation/MoneyJournal.hpp"

#include <stdexcept>
#include <utility>

namespace spacetrains::simulation {

namespace {
constexpr std::array<MoneyCategoryInfo, kMoneyCategories> kCategories {{
    {"cargo", "Cargo", "trade"},
    {"fuel", "Fuel", "trade"},
    {"fuel_buyback", "Fuel bought back", "trade"},
    {"provisions", "Provisions", "trade"},
    {"refits_repairs", "Refits and repairs", "trade"},
    {"household_goods", "Residents' goods", "residents"},
    {"household_spending", "Residents' spending", "residents"},
    {"passage_recovery", "Passages recovered", "residents"},
    {"wages", "Wages", "residents"},
    {"crew_wages", "Crew wages", "residents"},
    {"hazard_pay", "Hazard pay", "residents"},
    {"grant_science", "Science grants", "earth"},
    {"grant_settlement", "Settlement grants", "earth"},
    {"grant_relay", "Relay services", "earth"},
    {"grant_tourism", "Tourism and survey", "earth"},
    {"grant_other", "Other incomes", "earth"},
    {"surface_sales", "Surface sales", "earth"},
    {"surface_purchases", "Surface purchases", "earth"},
    {"visitor_spending", "Visitors' spending", "travel"},
    {"fares_traveller", "Fares (travellers)", "travel"},
    {"fares_employer", "Fares (employers)", "travel"},
    {"migrant_savings", "Migrants' savings", "travel"},
    {"savings_endowment", "Starting savings", "travel"},
    {"colony_investment", "Colony investment", "earth"},
    {"dividends", "Dividends", "owners"},
    {"capital_charge", "Capital charges", "owners"},
    {"ship_sale_cash", "Sold ships' cash", "owners"},
    {"faction_tax", "Faction tax", "factions"},
    {"faction_topup", "Faction top-ups", "factions"},
    {"insolvency_cover", "Insolvency cover", "factions"},
    {"money_controller", "Money controller", "factions"},
    {"liner_topup", "Liner top-ups", "factions"},
    {"emergency_premium", "Emergency premiums", "factions"},
    {"interest", "Interest", "factions"},
    {"hull_earth", "Hulls (Earth's share)", "investment"},
    {"hull_yard", "Hulls (yard's share)", "investment"},
    {"working_capital", "Working capital", "investment"},
    {"salvage", "Salvage", "investment"},
}};
}  // namespace

const MoneyCategoryInfo& money_category_info(MoneyCategory category) {
    return kCategories.at(static_cast<std::size_t>(category));
}

bool money_category_from_id(std::string_view id, MoneyCategory& out) {
    for (std::size_t i = 0; i < kCategories.size(); ++i) {
        if (kCategories[i].id == id) {
            out = static_cast<MoneyCategory>(i);
            return true;
        }
    }
    return false;
}

std::string_view account_kind_id(AccountKind kind) {
    switch (kind) {
        case AccountKind::Earth: return "earth";
        case AccountKind::Colony: return "colony";
        case AccountKind::Faction: return "faction";
        case AccountKind::Station: return "station";
        case AccountKind::Households: return "households";
        case AccountKind::Fleet: return "fleet";
    }
    return "unknown";
}

void MoneyJournal::reset(std::vector<AccountInfo> accounts) {
    accounts_ = std::move(accounts);
    seeds_.assign(accounts_.size(), 0.0);
    lifetime_.assign(accounts_.size(), CategoryFlows {});
    period_.assign(accounts_.size(), CategoryFlows {});
    history_.clear();
    lifetime_sectors_ = {};
    period_sectors_ = {};
    next_close_day_ = kPeriodDays;
}

int MoneyJournal::find(AccountKind kind, const std::string& id) const {
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        if (accounts_[i].kind == kind && accounts_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MoneyJournal::record(int from, int to, MoneyCategory category, double amount) {
    if (amount == 0.0) {
        return;
    }
    if (from < 0 || to < 0) {
        throw std::logic_error("money journal: transfer with an unknown account");
    }
    if (amount < 0.0) {
        std::swap(from, to);
        amount = -amount;
    }
    const auto c = static_cast<std::size_t>(category);
    const auto f = static_cast<std::size_t>(from);
    const auto t = static_cast<std::size_t>(to);
    lifetime_[f][c].out += amount;
    lifetime_[t][c].in += amount;
    period_[f][c].out += amount;
    period_[t][c].in += amount;
    const auto cell = static_cast<std::size_t>(accounts_[f].kind) * kAccountKinds + static_cast<std::size_t>(accounts_[t].kind);
    lifetime_sectors_[cell] += amount;
    period_sectors_[cell] += amount;
}

double MoneyJournal::journal_balance(int account) const {
    const auto a = static_cast<std::size_t>(account);
    double balance = seeds_.at(a);
    for (const auto& flow : lifetime_.at(a)) {
        balance += flow.in - flow.out;
    }
    return balance;
}

void MoneyJournal::close_period(JournalSample sample) {
    sample.flows = period_;
    sample.sectors = period_sectors_;
    history_.push_back(std::move(sample));
    period_.assign(accounts_.size(), CategoryFlows {});
    period_sectors_ = {};
    next_close_day_ += kPeriodDays;
}

}  // namespace spacetrains::simulation
