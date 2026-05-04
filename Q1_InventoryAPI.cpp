/**
 * ============================================================
 * QUESTION 1 — Annual Inventory Report API
 * Endpoint : GET /api/v1/inventory/details?from=YYYY-MM-DD&to=YYYY-MM-DD
 * Tech Stack: C++ | REST (Crow/Drogon-style) | SQLite (mocked in-memory)
 *             | nlohmann/json (manual serialisation shown)
 *
 * CLASS DESIGN
 * ─────────────────────────────────────────────────────────────
 *  1. Date               – value-object: parsing + comparison
 *  2. Inventory          – model: Inventory(id, purchase_dt, cost)
 *  3. InventoryDetail    – model: InventoryDetails(id, inventory_id, details)
 *  4. InventoryRecord    – projection (JOIN result) returned to client
 *  5. InventoryRepo      – Data-Access Layer (SQL lives here)
 *  6. InventoryService   – Business Logic (validation, aggregation)
 *  7. ApiResponse<T>     – generic HTTP envelope: {status, message, data}
 *  8. InventoryController– HTTP layer: parse → service → JSON response
 * ─────────────────────────────────────────────────────────────
 */

#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ─── 1. DATE value-object ────────────────────────────────────
struct Date {
    int year{}, month{}, day{};

    static Date parse(const std::string& s) {
        if (s.size() != 10 || s[4] != '-' || s[7] != '-')
            throw std::invalid_argument("Date must be YYYY-MM-DD, got: " + s);
        Date d;
        d.year  = std::stoi(s.substr(0, 4));
        d.month = std::stoi(s.substr(5, 2));
        d.day   = std::stoi(s.substr(8, 2));
        if (d.month < 1 || d.month > 12 || d.day < 1 || d.day > 31)
            throw std::invalid_argument("Invalid date values in: " + s);
        return d;
    }

    int asInt() const { return year * 10000 + month * 100 + day; }
    bool operator<=(const Date& o) const { return asInt() <= o.asInt(); }
    bool operator>=(const Date& o) const { return asInt() >= o.asInt(); }
};

// ─── 2 & 3. DOMAIN MODELS ─────────────────────────────────────
struct Inventory {
    int id{}; std::string purchase_dt; double cost{};
};

struct InventoryDetail {
    int id{}; int inventory_id{}; std::string inventory_details;
};

// JOIN projection sent to client
struct InventoryRecord {
    int id{}; int inventory_id{};
    std::string purchase_dt; double cost{};
    std::string details;
};

// ─── 4. REPOSITORY (Data-Access Layer) ───────────────────────
class InventoryRepo {
public:
    // SQL equivalent:
    //   SELECT i.id, i.purchase_dt, i.cost, d.id, d.inventory_details
    //   FROM   Inventory i JOIN InventoryDetails d ON d.inventory_id = i.id
    //   WHERE  i.purchase_dt BETWEEN :from AND :to
    //   ORDER  BY i.purchase_dt ASC
    std::vector<InventoryRecord> findByDateRange(const Date& from,
                                                  const Date& to) const {
        // ── Mock data (replace with real DB driver in production) ──
        static const std::vector<Inventory> invTable = {
            {1,"2024-01-15",1500.00}, {2,"2024-03-22",3200.50},
            {3,"2024-06-10", 800.75}, {4,"2024-09-05",4100.00},
            {5,"2025-01-20",2750.25}
        };
        static const std::vector<InventoryDetail> detTable = {
            {1,1,"Industrial pump – batch A"}, {2,2,"Server rack 2U"},
            {3,3,"Office supplies bundle"},    {4,4,"CNC machine components"},
            {5,5,"Networking cables Cat6"}
        };
        // ──────────────────────────────────────────────────────────
        std::vector<InventoryRecord> result;
        for (const auto& inv : invTable) {
            Date d = Date::parse(inv.purchase_dt);
            if (d >= from && d <= to) {
                for (const auto& det : detTable)
                    if (det.inventory_id == inv.id)
                        result.push_back({det.id, inv.id,
                                          inv.purchase_dt, inv.cost,
                                          det.inventory_details});
            }
        }
        std::sort(result.begin(), result.end(),
                  [](auto& a, auto& b){ return a.purchase_dt < b.purchase_dt; });
        return result;
    }
};

// ─── 5. SERVICE (Business Logic) ──────────────────────────────
class InventoryService {
    InventoryRepo repo_;
public:
    std::vector<InventoryRecord> getInventoryDetails(const std::string& fromStr,
                                                      const std::string& toStr) {
        Date from = Date::parse(fromStr);
        Date to   = Date::parse(toStr);
        if (to.asInt() < from.asInt())
            throw std::invalid_argument("from_date must be <= to_date.");
        return repo_.findByDateRange(from, to);
    }
};

// ─── 6. GENERIC API RESPONSE WRAPPER ─────────────────────────
template<typename T>
struct ApiResponse { int httpStatus; std::string message; T data;
    std::string toJson() const;
};

template<>
std::string ApiResponse<std::vector<InventoryRecord>>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"status\": " << httpStatus
       << ",\n  \"message\": \"" << message << "\","
       << "\n  \"data\": [\n";
    for (size_t i = 0; i < data.size(); ++i) {
        const auto& r = data[i];
        os << "    {\"id\":" << r.id << ",\"inventory_id\":" << r.inventory_id
           << ",\"purchase_dt\":\"" << r.purchase_dt << "\",\"cost\":"
           << r.cost << ",\"details\":\"" << r.details << "\"}";
        if (i+1 < data.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n}";
    return os.str();
}

template<>
std::string ApiResponse<std::string>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"status\": " << httpStatus
       << ",\n  \"message\": \"" << message << "\","
       << "\n  \"data\": \"" << data << "\"\n}";
    return os.str();
}

// ─── 7. CONTROLLER (HTTP Layer) ──────────────────────────────
class InventoryController {
    InventoryService svc_;
public:
    // Route: GET /api/v1/inventory/details?from=...&to=...
    std::string handleGetInventoryDetails(const std::string& from,
                                           const std::string& to) {
        try {
            auto records = svc_.getInventoryDetails(from, to);
            if (records.empty())
                return ApiResponse<std::string>{404,
                    "No records found for the given date range.",""}.toJson();
            return ApiResponse<std::vector<InventoryRecord>>{200,
                "Inventory details fetched successfully.", records}.toJson();
        } catch (const std::invalid_argument& e) {
            return ApiResponse<std::string>{400, e.what(), ""}.toJson();
        } catch (...) {
            return ApiResponse<std::string>{500, "Internal server error.",""}.toJson();
        }
    }
};

// ─── MAIN: simulate HTTP calls ────────────────────────────────
int main() {
    InventoryController ctrl;

    std::cout << "=== Q1: GET /api/v1/inventory/details ===\n\n";

    std::cout << "[TEST 1] Valid range 2024-01-01 to 2024-12-31\n";
    std::cout << ctrl.handleGetInventoryDetails("2024-01-01","2024-12-31") << "\n\n";

    std::cout << "[TEST 2] No results range 2023-01-01 to 2023-12-31\n";
    std::cout << ctrl.handleGetInventoryDetails("2023-01-01","2023-12-31") << "\n\n";

    std::cout << "[TEST 3] from > to (bad range)\n";
    std::cout << ctrl.handleGetInventoryDetails("2024-12-31","2024-01-01") << "\n\n";

    std::cout << "[TEST 4] Malformed date\n";
    std::cout << ctrl.handleGetInventoryDetails("20-Jan-2024","2024-12-31") << "\n";
    return 0;
}