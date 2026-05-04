/**
 * ============================================================
 * QUESTION 3 — High-Performance Posts API (Timeout Fix)
 * Endpoint : GET /getPostsUploaded?page=1&page_size=20&last_id=0
 *
 * PROBLEM: getPostsUploaded() times out because it fetches the
 * entire Posts table in one query on a huge dataset.
 *
 * STRATEGY: Keyset (Cursor) Pagination  +  Redis-style In-Memory Cache
 * ─────────────────────────────────────────────────────────────
 * WHY THESE TWO?
 *  • Keyset Pagination  – O(log N) via indexed seek on id; avoids
 *    expensive OFFSET scans; stable under concurrent inserts.
 *  • In-Memory Cache    – repeated calls for the same page are served
 *    instantly without hitting the DB at all.  TTL prevents stale data.
 *
 * Tech Stack: C++ | REST (Crow-style) | PostgreSQL (mocked) | STL unordered_map
 *
 * CLASS DESIGN
 * ─────────────────────────────────────────────────────────────
 *  1. Post              – model: Posts(id, post_by, post_dt, post_details)
 *  2. PageRequest       – value-object: page_size + cursor (last_id)
 *  3. PageResponse<T>   – paginated envelope with next_cursor
 *  4. CacheEntry        – cached data + expiry timestamp
 *  5. InMemoryCache     – TTL-based cache (std::unordered_map)
 *  6. PostRepo          – DAL: keyset-paginated SQL query
 *  7. PostService       – cache-aside logic: cache hit → return;
 *                          miss → repo → store → return
 *  8. ApiResponse<T>    – generic HTTP envelope
 *  9. PostController    – HTTP layer / route handler
 * ─────────────────────────────────────────────────────────────
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;
using TP    = std::chrono::time_point<Clock>;

// ─── 1. POST MODEL ────────────────────────────────────────────
struct Post {
    int         id{};
    std::string post_by;
    std::string post_dt;
    std::string post_details;
};

// ─── 2. PAGE REQUEST value-object ────────────────────────────
struct PageRequest {
    int page_size{20};   // number of rows per page
    int last_id{0};      // cursor: id of last item seen (0 = first page)

    static PageRequest parse(const std::string& pageSizeStr,
                              const std::string& lastIdStr) {
        PageRequest r;
        try { r.page_size = std::stoi(pageSizeStr); } catch (...) {}
        try { r.last_id   = std::stoi(lastIdStr);   } catch (...) {}
        if (r.page_size < 1 || r.page_size > 100)
            throw std::invalid_argument("page_size must be 1-100.");
        return r;
    }

    std::string cacheKey() const {
        return "posts:lid=" + std::to_string(last_id)
             + ":ps="  + std::to_string(page_size);
    }
};

// ─── 3. PAGE RESPONSE ─────────────────────────────────────────
template<typename T>
struct PageResponse {
    std::vector<T> items;
    int            next_cursor{0};   // id of last item; 0 = no more pages
    bool           has_more{false};

    std::string toJson() const;
};

template<>
std::string PageResponse<Post>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"items\": [\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& p = items[i];
        os << "    {\"id\":" << p.id
           << ",\"post_by\":\""     << p.post_by     << "\""
           << ",\"post_dt\":\""     << p.post_dt     << "\""
           << ",\"post_details\":\"" << p.post_details << "\"}";
        if (i+1 < items.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n"
       << "  \"next_cursor\": " << next_cursor << ",\n"
       << "  \"has_more\": "    << (has_more ? "true" : "false") << "\n"
       << "}";
    return os.str();
}

// ─── 4 & 5. TTL IN-MEMORY CACHE ──────────────────────────────
struct CacheEntry {
    std::string value;
    TP          expires_at;
};

class InMemoryCache {
    std::unordered_map<std::string, CacheEntry> store_;
    std::chrono::seconds ttl_;

public:
    explicit InMemoryCache(int ttlSeconds = 60)
        : ttl_(std::chrono::seconds(ttlSeconds)) {}

    void set(const std::string& key, const std::string& value) {
        store_[key] = {value, Clock::now() + ttl_};
    }

    // Returns "" on miss or expiry
    std::string get(const std::string& key) {
        auto it = store_.find(key);
        if (it == store_.end() || Clock::now() > it->second.expires_at) {
            store_.erase(key);  // evict stale entry
            return "";
        }
        return it->second.value;
    }
};

// ─── 6. POST REPOSITORY (Data-Access Layer) ───────────────────
class PostRepo {
    // Mock dataset – simulates 1000s of rows in the real DB
    std::vector<Post> table_;
public:
    PostRepo() {
        // Generate 50 mock posts
        for (int i = 1; i <= 50; ++i) {
            table_.push_back({i,
                "user_" + std::to_string(i % 10 + 1),
                "2024-0" + std::to_string(i % 9 + 1) + "-"
                    + (i < 10 ? "0" : "") + std::to_string(i),
                "Post content number " + std::to_string(i)});
        }
    }

    /**
     * Keyset paginated query — SQL equivalent:
     *   SELECT id, post_by, post_dt, post_details
     *   FROM   Posts
     *   WHERE  id > :last_id
     *   ORDER  BY id ASC
     *   LIMIT  :page_size + 1;           ← +1 to detect has_more
     *
     * The +1 trick: fetch one extra row; if we get page_size+1 rows
     * there IS a next page — drop the extra row before returning.
     */
    std::vector<Post> fetchPage(const PageRequest& req) const {
        std::vector<Post> result;
        int limit = req.page_size + 1;   // +1 to probe for next page

        for (const auto& p : table_) {
            if (p.id > req.last_id) {
                result.push_back(p);
                if ((int)result.size() == limit) break;
            }
        }
        return result;
    }
};

// ─── 7. POST SERVICE (Cache-Aside Logic) ─────────────────────
class PostService {
    PostRepo      repo_;
    InMemoryCache cache_{30};   // 30-second TTL

public:
    PageResponse<Post> getPostsUploaded(const PageRequest& req) {
        const std::string key = req.cacheKey();

        // ── Cache Hit ─────────────────────────────────────────
        std::string cached = cache_.get(key);
        if (!cached.empty()) {
            std::cout << "[CACHE HIT] key=" << key << "\n";
            // In a real app, deserialise cached JSON back to PageResponse.
            // Here we show the mechanism; production uses protobuf / msgpack.
            PageResponse<Post> dummy;
            dummy.items.push_back({0,"","","<served from cache>"});
            return dummy;
        }

        // ── Cache Miss → DB query ─────────────────────────────
        std::cout << "[CACHE MISS] key=" << key << " → querying DB\n";
        auto rows = repo_.fetchPage(req);

        PageResponse<Post> resp;
        resp.has_more = ((int)rows.size() > req.page_size);
        if (resp.has_more) rows.pop_back();   // remove probe row

        resp.items       = rows;
        resp.next_cursor = rows.empty() ? 0 : rows.back().id;

        // Store serialised result in cache
        cache_.set(key, resp.toJson());
        return resp;
    }
};

// ─── 8. GENERIC API RESPONSE ─────────────────────────────────
template<typename T>
struct ApiResponse {
    int httpStatus; std::string message; T data;
    std::string toJson() const;
};

template<>
std::string ApiResponse<PageResponse<Post>>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"status\": "    << httpStatus
       << ",\n  \"message\": \"" << message << "\","
       << "\n  \"data\": "       << data.toJson() << "\n}";
    return os.str();
}

template<>
std::string ApiResponse<std::string>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"status\": "    << httpStatus
       << ",\n  \"message\": \"" << message << "\","
       << "\n  \"data\": \""     << data    << "\"\n}";
    return os.str();
}

// ─── 9. CONTROLLER ────────────────────────────────────────────
class PostController {
    PostService svc_;
public:
    // Route: GET /getPostsUploaded?page_size=5&last_id=0
    std::string handleGetPostsUploaded(const std::string& pageSizeStr,
                                        const std::string& lastIdStr) {
        try {
            auto req  = PageRequest::parse(pageSizeStr, lastIdStr);
            auto resp = svc_.getPostsUploaded(req);
            return ApiResponse<PageResponse<Post>>{200,
                "Posts fetched successfully.", resp}.toJson();
        } catch (const std::invalid_argument& e) {
            return ApiResponse<std::string>{400, e.what(), ""}.toJson();
        } catch (...) {
            return ApiResponse<std::string>{500,"Internal server error.",""}.toJson();
        }
    }
};

// ─── MAIN ─────────────────────────────────────────────────────
int main() {
    PostController ctrl;
    std::cout << "=== Q3: GET /getPostsUploaded (Keyset Pagination + Cache) ===\n\n";

    // Page 1
    std::cout << "--- Page 1 (last_id=0, page_size=5) ---\n";
    std::cout << ctrl.handleGetPostsUploaded("5","0") << "\n\n";

    // Page 1 again — should be a cache hit
    std::cout << "--- Page 1 repeated (cache hit expected) ---\n";
    ctrl.handleGetPostsUploaded("5","0");
    std::cout << "\n";

    // Page 2 — advance cursor to last seen id
    std::cout << "--- Page 2 (last_id=5, page_size=5) ---\n";
    std::cout << ctrl.handleGetPostsUploaded("5","5") << "\n\n";

    // Page 3
    std::cout << "--- Page 3 (last_id=10, page_size=5) ---\n";
    std::cout << ctrl.handleGetPostsUploaded("5","10") << "\n\n";

    // Bad page_size
    std::cout << "--- Invalid page_size ---\n";
    std::cout << ctrl.handleGetPostsUploaded("999","0") << "\n";
    return 0;
}