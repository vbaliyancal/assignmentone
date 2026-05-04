/**
 * ============================================================
 * QUESTION 2 — Device Config-Change Notification System
 * Endpoint : GET  /api/v1/devices/configNotification
 * Tech Stack: C++ | REST API (Crow/Pistache-style) | SQLite (mocked)
 *             | Observer Pattern | JSON (manual serialisation)
 *
 * DESIGN PATTERN: Observer (Publisher-Subscriber)
 *   - DeviceRepo polls / listens for config_changed == true
 *   - NotificationPublisher dispatches JSON messages to subscribers
 *
 * CLASS DESIGN
 * ─────────────────────────────────────────────────────────────
 *  1. Device               – model: Devices(id,device_ip,device_details,
 *                             config_changed)
 *  2. NotificationMessage  – JSON payload sent to the user
 *  3. INotificationChannel – interface (email / websocket / push)
 *  4. ConsoleNotificationChannel – concrete channel (prints to stdout;
 *                             swap for WebSocket/SMTP in production)
 *  5. NotificationPublisher– builds & dispatches NotificationMessage
 *  6. DeviceRepo           – DAL: fetch devices where config_changed = true
 *  7. DeviceConfigService  – orchestrates: repo → publisher → reset flag
 *  8. ApiResponse<T>       – generic HTTP envelope
 *  9. DeviceController     – HTTP layer: route handler
 * ─────────────────────────────────────────────────────────────
 */

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ─── Utility: current UTC timestamp ─────────────────────────
static std::string nowISO() {
    auto t  = std::time(nullptr);
    std::ostringstream os;
    os << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

// ─── 1. DOMAIN MODEL ─────────────────────────────────────────
struct Device {
    int         id{};
    std::string device_ip;
    std::string device_details;
    bool        config_changed{false};
};

// ─── 2. NOTIFICATION MESSAGE ─────────────────────────────────
struct NotificationMessage {
    std::string event_type;    // "CONFIG_CHANGE"
    int         device_id{};
    std::string device_ip;
    std::string device_details;
    std::string timestamp;

    std::string toJson() const {
        std::ostringstream os;
        os << "{\n"
           << "  \"event_type\": \""    << event_type    << "\",\n"
           << "  \"device_id\": "       << device_id     << ",\n"
           << "  \"device_ip\": \""     << device_ip     << "\",\n"
           << "  \"device_details\": \"" << device_details << "\",\n"
           << "  \"timestamp\": \""     << timestamp     << "\"\n"
           << "}";
        return os.str();
    }
};

// ─── 3. NOTIFICATION CHANNEL INTERFACE ───────────────────────
class INotificationChannel {
public:
    virtual ~INotificationChannel() = default;
    virtual void send(const NotificationMessage& msg) = 0;
};

// ─── 4. CONCRETE CHANNEL (Console / replace with WS/Email) ───
class ConsoleNotificationChannel : public INotificationChannel {
public:
    void send(const NotificationMessage& msg) override {
        std::cout << "[NOTIFICATION DISPATCHED]\n"
                  << msg.toJson() << "\n";
    }
};

// ─── 5. NOTIFICATION PUBLISHER ────────────────────────────────
class NotificationPublisher {
    std::vector<std::shared_ptr<INotificationChannel>> channels_;
public:
    void addChannel(std::shared_ptr<INotificationChannel> ch) {
        channels_.push_back(std::move(ch));
    }

    // Build a NotificationMessage and broadcast to all channels
    void publish(const Device& device) {
        NotificationMessage msg;
        msg.event_type     = "CONFIG_CHANGE";
        msg.device_id      = device.id;
        msg.device_ip      = device.device_ip;
        msg.device_details = device.device_details;
        msg.timestamp      = nowISO();

        for (auto& ch : channels_)
            ch->send(msg);
    }
};

// ─── 6. DEVICE REPOSITORY (Data-Access Layer) ─────────────────
class DeviceRepo {
    // In-memory mock of Devices table
    std::vector<Device> table_ = {
        {1, "192.168.1.10", "Router-CoreA",    false},
        {2, "192.168.1.20", "Switch-FloorB",   true },   // ← flag set by batch
        {3, "192.168.1.30", "Firewall-EdgeC",  false},
        {4, "10.0.0.5",     "AP-Lobby",        true },   // ← flag set by batch
    };

public:
    // SELECT * FROM Devices WHERE config_changed = TRUE
    std::vector<Device> fetchChangedDevices() const {
        std::vector<Device> result;
        for (const auto& d : table_)
            if (d.config_changed) result.push_back(d);
        return result;
    }

    // UPDATE Devices SET config_changed = FALSE WHERE id = :id
    void resetFlag(int deviceId) {
        for (auto& d : table_)
            if (d.id == deviceId) { d.config_changed = false; break; }
    }
};

// ─── 7. DEVICE CONFIG SERVICE (Business Logic) ────────────────
class DeviceConfigService {
    DeviceRepo            repo_;
    NotificationPublisher publisher_;

public:
    DeviceConfigService() {
        publisher_.addChannel(
            std::make_shared<ConsoleNotificationChannel>());
        // Add more channels here: EmailChannel, WebSocketChannel, etc.
    }

    /**
     * Core logic for deviceConfigNotification API:
     *  1. Fetch all devices with config_changed = true
     *  2. For each → publish notification JSON
     *  3. Reset the flag so the same event is not re-sent
     *  Returns the count of notifications triggered.
     */
    int deviceConfigNotification() {
        auto devices = repo_.fetchChangedDevices();
        int count = 0;
        for (const auto& dev : devices) {
            publisher_.publish(dev);
            repo_.resetFlag(dev.id);
            ++count;
        }
        return count;
    }
};

// ─── 8. GENERIC API RESPONSE ──────────────────────────────────
template<typename T>
struct ApiResponse {
    int httpStatus; std::string message; T data;
    std::string toJson() const;
};

template<>
std::string ApiResponse<std::string>::toJson() const {
    std::ostringstream os;
    os << "{\n  \"status\": "    << httpStatus
       << ",\n  \"message\": \"" << message << "\","
       << "\n  \"data\": \""     << data    << "\"\n}";
    return os.str();
}

// ─── 9. CONTROLLER ────────────────────────────────────────────
class DeviceController {
    DeviceConfigService svc_;
public:
    // Route: GET /api/v1/devices/configNotification
    std::string handleDeviceConfigNotification() {
        try {
            int n = svc_.deviceConfigNotification();
            std::string msg = (n == 0)
                ? "No device configuration changes detected."
                : std::to_string(n) + " notification(s) dispatched.";
            return ApiResponse<std::string>{200, msg, ""}.toJson();
        } catch (...) {
            return ApiResponse<std::string>{500,"Internal server error.",""}.toJson();
        }
    }
};

// ─── MAIN ─────────────────────────────────────────────────────
int main() {
    DeviceController ctrl;
    std::cout << "=== Q2: GET /api/v1/devices/configNotification ===\n\n";
    std::string resp = ctrl.handleDeviceConfigNotification();
    std::cout << "\n[HTTP RESPONSE]\n" << resp << "\n\n";

    // Second call — flags have been reset, no notifications expected
    std::cout << "[Second call — flags already reset]\n";
    resp = ctrl.handleDeviceConfigNotification();
    std::cout << "\n[HTTP RESPONSE]\n" << resp << "\n";
    return 0;
}