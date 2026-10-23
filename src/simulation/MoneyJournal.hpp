// The money journal: every transfer between accounts, by category (docs/modules/economy.md,
// "Money journal"). It only records: the balances themselves stay where they are (Simulation's
// accounts), and each money move tells the journal what it was. An account's balance always equals
// its seed + everything it received - everything it paid (tests/economy_test checks it).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spacetrains::simulation {

// One category names a flow; the payer books it as an expense, the payee as an income.
enum class MoneyCategory : std::uint8_t {
    Cargo,               // ships buy cargo at stations, and are paid for what they deliver
    Fuel,                // ships buy propellant
    FuelBuyback,         // a yard pays a ship for propellant that no longer fits after a refit
    Provisions,          // ships buy their crew's provisions
    RefitsRepairs,       // refits and repairs at a yard
    HouseholdGoods,      // residents pay for the goods they use up
    HouseholdSpending,   // residents' spending beyond that (services, rent, local taxes)
    PassageRecovery,     // a station recovers passages it paid through its residents' prices
    Wages,               // a station pays its residents
    CrewWages,           // ships pay their crews (residents of the home station)
    HazardPay,           // ships pay their crews for radiation dose
    GrantScience,        // station incomes by kind (station_incomes.csv)
    GrantSettlement,
    GrantRelay,
    GrantTourism,
    GrantOther,
    SurfaceSales,        // Earth or a colony sells goods to a station (its exports)
    SurfacePurchases,    // Earth or a colony buys goods from a station (its imports)
    VisitorSpending,     // visitors spend during their stay, out of their home's account
    FaresTraveller,      // travellers pay ships their fares
    FaresEmployer,       // a station pays its staff's or settlers' passages
    MigrantSavings,      // savings move with migrants, settlers, recruits, emigrants
    SavingsEndowment,    // residents' starting savings, brought from Earth
    ColonyInvestment,    // Earth invests in the colonies
    Dividends,           // ships send their cash above the working reserve home
    CapitalCharge,       // ships pay their hull's capital charge to the home station
    ShipSaleCash,        // a sold ship's cash (or debt) goes home
    FactionTax,          // a faction takes its station's cash above the ceiling
    FactionTopup,        // a faction tops up its station below the floor
    InsolvencyCover,     // a faction covers its station in the red
    MoneyController,     // the money-supply controller (off since the economy overhaul)
    LinerTopup,          // a faction keeps its liners' cash at the reserve
    EmergencyPremium,    // a faction pays the premium on an emergency delivery
    Interest,            // a faction in debt pays Earth's lenders
    HullEarth,           // a faction buys a hull: Earth's share
    HullYard,            // ... and its yard's share
    WorkingCapital,      // a faction gives a new ship its working capital
    Salvage,             // Earth buys a sold ship's hull
    Count
};
inline constexpr std::size_t kMoneyCategories = static_cast<std::size_t>(MoneyCategory::Count);

struct MoneyCategoryInfo {
    std::string_view id;      // stable: saves and JSON
    std::string_view label;   // for tables and the UI
    std::string_view group;   // trade, residents, earth, travel, owners, factions, investment
};
[[nodiscard]] const MoneyCategoryInfo& money_category_info(MoneyCategory category);
[[nodiscard]] bool money_category_from_id(std::string_view id, MoneyCategory& out);

enum class AccountKind : std::uint8_t { Earth, Colony, Faction, Station, Households, Fleet };
inline constexpr std::size_t kAccountKinds = 6;
// Money between sectors (account kinds), payer x payee: [from * kAccountKinds + to].
using SectorMatrix = std::array<double, kAccountKinds * kAccountKinds>;
[[nodiscard]] std::string_view account_kind_id(AccountKind kind);

struct AccountInfo {
    AccountKind kind {AccountKind::Earth};
    std::string id;           // economy, faction or station id ("earth" for Earth)
    std::string name;
    std::string faction_id;   // the owner (stations, households, fleets)
    [[nodiscard]] std::string key() const { return std::string(account_kind_id(kind)) + ":" + id; }
};

struct MoneyFlow {
    double in {0.0};
    double out {0.0};
};
using CategoryFlows = std::array<MoneyFlow, kMoneyCategories>;

// The people behind the accounts at a sample (for the statistics' people tables).
struct StationPeopleSample {
    double population {0.0};
    double workers {0.0};
    double jobs {0.0};
    double wage_cr_per_day {0.0};
};

// One closed period (a month): balances at its end and the flows during it.
struct JournalSample {
    double day {0.0};
    std::vector<double> balances;          // per account
    std::vector<CategoryFlows> flows;      // per account
    std::vector<StationPeopleSample> stations;   // per station (universe order)
    std::vector<double> colony_people;     // per colony account
    std::vector<double> station_profit;    // per station: lifetime operating profit at the sample
    SectorMatrix sectors {};               // money between sectors during the period
};

class MoneyJournal {
public:
    static constexpr double kPeriodDays = 30.0;

    void reset(std::vector<AccountInfo> accounts);
    [[nodiscard]] const std::vector<AccountInfo>& accounts() const { return accounts_; }
    [[nodiscard]] int find(AccountKind kind, const std::string& id) const;   // -1 if none

    // `amount` moved from one account to another. A negative amount moves the other way.
    void record(int from, int to, MoneyCategory category, double amount);
    void set_seed(int account, double balance) { seeds_.at(static_cast<std::size_t>(account)) = balance; }
    [[nodiscard]] double seed(int account) const { return seeds_.at(static_cast<std::size_t>(account)); }

    [[nodiscard]] const CategoryFlows& lifetime(int account) const { return lifetime_.at(static_cast<std::size_t>(account)); }
    [[nodiscard]] const CategoryFlows& period(int account) const { return period_.at(static_cast<std::size_t>(account)); }
    [[nodiscard]] const SectorMatrix& lifetime_sectors() const { return lifetime_sectors_; }
    [[nodiscard]] const SectorMatrix& period_sectors() const { return period_sectors_; }
    // The balance the journal accounts for: seed + received - paid.
    [[nodiscard]] double journal_balance(int account) const;

    // Close the running period: append a sample and start the next one.
    void close_period(JournalSample sample);
    [[nodiscard]] double next_close_day() const { return next_close_day_; }
    void set_next_close_day(double day) { next_close_day_ = day; }
    [[nodiscard]] const std::vector<JournalSample>& history() const { return history_; }

    // Persistence (SimulationPersistence.cpp) restores these directly.
    std::vector<double>& seeds() { return seeds_; }
    std::vector<CategoryFlows>& lifetime_mutable() { return lifetime_; }
    std::vector<CategoryFlows>& period_mutable() { return period_; }
    std::vector<JournalSample>& history_mutable() { return history_; }
    SectorMatrix& lifetime_sectors_mutable() { return lifetime_sectors_; }
    SectorMatrix& period_sectors_mutable() { return period_sectors_; }

private:
    std::vector<AccountInfo> accounts_;
    std::vector<double> seeds_;
    std::vector<CategoryFlows> lifetime_;
    std::vector<CategoryFlows> period_;
    std::vector<JournalSample> history_;
    SectorMatrix lifetime_sectors_ {};
    SectorMatrix period_sectors_ {};
    double next_close_day_ {kPeriodDays};
};

}  // namespace spacetrains::simulation
