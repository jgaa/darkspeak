#include "transport.h"
#include "service.h"
#ifdef DARKSPEAK_WITH_TEST_MCP
#include "test_support.h"
#endif
#include <QJsonDocument>
#include <QMetaObject>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <climits>
#include <mutex>
#include <yahat/Mcp.h>

namespace ds::mcp {
namespace json = boost::json;
namespace net = boost::asio;
namespace {
// Reject object growth and deeply nested input before the library dispatcher.
// HTTP parser limits bound the parse allocation itself.
bool boundedJson(const json::value& value, unsigned depth = 0) {
    if (depth > 16)
        return false;
    if (value.is_string())
        return value.as_string().size() <= 65536;
    if (value.is_object()) {
        if (value.as_object().size() > 64)
            return false;
        for (const auto& item : value.as_object())
            if (item.key().size() > 128 || !boundedJson(item.value(), depth + 1))
                return false;
    }
    if (value.is_array()) {
        if (value.as_array().size() > 64)
            return false;
        for (const auto& item : value.as_array())
            if (!boundedJson(item, depth + 1))
                return false;
    }
    return true;
}
json::object schema(const Tool& tool) {
    json::object properties;
    json::array required;
    for (const auto& field : tool.fields) {
        json::object entry{{"type", "string"}, {"maxLength", 1024}};
        if (field == "identity" || field == "contact" || field == "operation")
            entry = {{"type", "string"}, {"minLength", 36}, {"maxLength", 36}};
        if (field == "text")
            entry = {{"type", "string"}, {"minLength", 1}, {"maxLength", 65536}};
        if (field == "name")
            entry = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
        if (field == "message")
            entry = {{"type", "string"}, {"maxLength", 4096}};
        if (field == "cursor")
            entry = {{"type", "string"}, {"maxLength", 4096}};
        if (field == "idempotency_key")
            entry = {{"type", "string"},
                     {"minLength", 3},
                     {"maxLength", 128},
                     {"description", "Server-issued retry_epoch from status, colon, then a unique ASCII "
                                     "token. Reuse unchanged for retries; retained for 24 hours."}};
        if (field == "contacts")
            entry = {{"type", "array"},
                     {"minItems", 1},
                     {"maxItems", 8},
                     {"uniqueItems", true},
                     {"items", json::object{{"type", "string"}, {"minLength", 36}, {"maxLength", 36}}}};
        if (field == "limit")
            entry = {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}, {"default", 32}};
        if (field == "after")
            entry = {{"type", "integer"}, {"minimum", 0}, {"maximum", INT_MAX}, {"default", 0}};
        if (field == "request")
            entry = {{"type", "string"},
                     {"minLength", 64},
                     {"maxLength", 64},
                     {"description", "Opaque request handle returned by contact_requests."}};
        if (field == "conversation")
            entry = {{"type", "string"}, {"minLength", 36}, {"maxLength", 36}};
#ifdef DARKSPEAK_WITH_TEST_MCP
        if (field == "transfer")
            entry = {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}, {"pattern", "^[0-9a-f]{32}$"}};
        if (field == "asset")
            entry = {{"type", "string"}, {"enum", json::array{"earth.jpg", "apollo11.jpg", "sos.ogg"}}};
        if (field == "decision")
            entry = {{"type", "string"},
                     {"enum", json::array{"Allow once", "Allow always", "Deny", "Deny always"}}};
        if (field == "scope")
            entry = {{"type", "string"}, {"enum", json::array{"Selected contacts", "All contacts"}}};
#endif
        properties[field.toStdString()] = entry;
        if (tool.required.contains(field))
            required.push_back(json::string(field.toStdString()));
    }
    return {{"type", "object"},
            {"properties", properties},
            {"required", required},
            {"additionalProperties", false}};
}
} // namespace
struct Transport::Impl {
    Service& service;
#ifdef DARKSPEAK_WITH_TEST_MCP
    TestSupport* controller = nullptr;
#endif
    yahat::HttpConfig config;
    std::unique_ptr<yahat::HttpServer> http;
    std::shared_ptr<yahat::McpServer> mcp;
    bool started = false;
    std::mutex rate_mutex;
    std::chrono::steady_clock::time_point rate_tick = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point admission_tick = std::chrono::steady_clock::now();
    double tokens = 20, ordinary_tokens = 540, control_tokens = 60;
    struct PrincipalBudget {
        double ordinary = 100, control = 20;
        std::chrono::steady_clock::time_point tick = std::chrono::steady_clock::now();
    };
    std::map<QString, PrincipalBudget> principal_budgets;
    QString authenticate(const std::string& bearer) {
#ifdef DARKSPEAK_WITH_TEST_MCP
        if (controller)
            return controller->authenticate(bearer);
#endif
        return service.authenticate(bearer);
    }
    Impl(Service& owner, const QString& address, int port
#ifdef DARKSPEAK_WITH_TEST_MCP
         ,
         TestSupport* control
#endif
         )
        : service(owner) {
#ifdef DARKSPEAK_WITH_TEST_MCP
        controller = control;
#endif
        config.http_endpoint = address.toStdString();
        config.http_port = std::to_string(port);
        config.num_http_threads = 2;
        config.enable_http_basic_auth = false;
        config.auto_handle_cors = false;
        config.http_io_timeout = 10;
        config.max_request_header_size = 16 * 1024;
        config.max_request_body_size = 256 * 1024;
        config.max_decompressed_size = 256 * 1024;
        // Consumer patch on the pinned source adds admission before session
        // allocation and rejects compression before any decompression occurs.
        config.max_connections = 64;
        config.reject_compressed_requests = true;
        http = std::make_unique<yahat::HttpServer>(config, [this](const yahat::AuthReq& req) {
            yahat::Auth auth;
            const auto key = authenticate(std::string(req.auth_header));
            auth.access = !key.isEmpty();
            auth.account = key.toStdString();
            return auth;
        });
        yahat::McpServer::Options options;
        options.max_input_bytes = 256 * 1024;
        options.max_output_bytes = 1024 * 1024;
        options.max_help_bytes = 32 * 1024;
        options.page_size = 64;
        options.max_concurrent_calls = 32;
        options.calls_per_minute = 600;
        options.reserved_tool = "operation_status";
        options.reserved_slots = 8;
        options.reserved_calls_per_minute = 60;
        mcp = std::make_shared<yahat::McpServer>(yahat::McpServer::Identity{"darkspeak", DARKSPEAK_VERSION},
                                                 options);
        auto catalog = tools();
#ifdef DARKSPEAK_WITH_TEST_MCP
        if (!controller && !service.testSupportActive())
            catalog.removeIf([](const auto& tool) { return TestSupport::isOperation(tool.name); });
        if (controller) {
            catalog = TestSupport::controllerTools();
            options.reserved_tool = "controller_decide";
            options.reserved_slots = 8;
            // Decisions have a separate listener and cannot be starved by ordinary
            // calls.
            mcp = std::make_shared<yahat::McpServer>(
                yahat::McpServer::Identity{"darkspeak-controller", DARKSPEAK_VERSION}, options);
        }
#endif
        for (const auto& tool : catalog) {
            yahat::ToolDescriptor descriptor;
            descriptor.name = tool.name.toStdString();
            descriptor.input_schema = schema(tool);
            descriptor.description = "DarkSpeak " + tool.name.toStdString() + ". Requires " +
                                     tool.capability.toStdString() +
                                     (tool.gated ? " and operator approval; use help for retry semantics."
                                                 : "; use help for constraints.");
            descriptor.annotations = {{"readOnlyHint", !tool.mutates},
                                      {"destructiveHint", tool.name == "contact_remove"},
                                      {"idempotentHint", true},
                                      {"openWorldHint", tool.mutates}};
            descriptor.help["authorization"] =
                "Every request requires a bearer API key, the documented capability "
                "and authorized identity. "
                "read_only restricts mutations. Sensitive reads and all mutations "
                "require exact-argument "
                "approval; contact Ask requires a fresh decision and matching "
                "persistent denials win. No "
                "content previews in metadata. Full arguments appear only in the "
                "local approval view.";
            descriptor.help["usage"] = "Identity/contact IDs are canonical UUIDs. List pages contain 1–64 "
                                       "items (default 32); pass "
                                       "returned cursor to continue. Profiles and presence read local state "
                                       "only. Messages contain "
                                       "at most 65536 UTF-8 bytes. message_send targets 1–8 contacts and "
                                       "respects peer preferences. Queued sends initiate a connection to an "
                                       "accepted, unblocked disconnected contact when its identity is online; "
                                       "an offline identity remains offline. "
                                       "contact_requests returns opaque request handles for approve/reject. "
                                       "Completion of send "
                                       "means local acceptance, not remote delivery.";
            descriptor.help["retries"] =
                "Approval-gated calls require idempotency_key: status.retry_epoch + "
                "':' + a unique printable "
                "ASCII token, at most 128 bytes. Reuse the same arguments and key on "
                "disconnect. A retry "
                "returns sanitized operation state, never repeats approval or "
                "effects and never returns "
                "sensitive content. Use a new approved call to read contents again. "
                "Records last 24 hours; "
                "expired keys are rejected. Changed arguments cause "
                "IdempotencyConflict. "
                "IndeterminateOperation requires recovery; do not retry under a new "
                "key. operation_status "
                "requires the original key, permissions and identity/contact access.";
#ifdef DARKSPEAK_WITH_TEST_MCP
            if (TestSupport::isOperation(tool.name)) {
                descriptor.help["usage"] =
                    "Isolated test-build operations through production authorization "
                    "and domain services. "
                    "bio_probe starts the normal public-bio request for sharing_id; "
                    "bio_preview reads its "
                    "local result. file_offer imports one of the five fixed test "
                    "fixture names, never "
                    "a client path. file_accept accepts an incoming 32-hex transfer "
                    "reference for contact. file_reject rejects an offered incoming transfer. "
                    "file_cancel_incoming and file_cancel_outgoing cancel only the named direction through normal domain services. "
                    "file_resume invokes the normal incoming failed-file resume action. "
                    "files returns at most 64 sensitive file records for contact. "
                    "Test file records include thumbnail_for (empty for primary files) and preview_available; "
                    "the latter reports only local, preference-gated thumbnail availability, never image bytes or paths. "
                    "Offers/acceptance complete at local admission; wait for remote "
                    "terminal state. "
                    "All require exact-argument approval and idempotency_key. No "
                    "functionality without "
                    "the explicit isolated bootstrap; these extensions are absent from "
                    "production builds.";
            }
            if (controller) {
                descriptor.description = "Isolated instance controller: " + tool.name.toStdString();
                descriptor.help.clear();
                descriptor.help["authorization"] =
                    "Separate controller credential and loopback listener, only in "
                    "explicitly isolated "
                    "test instances. Ordinary keys never authenticate here. Decisions "
                    "invoke the "
                    "production approval manager and cannot bypass capability/contact "
                    "denials.";
                descriptor.help["usage"] =
                    "controller_pending exposes complete local arguments (sensitive; "
                    "never log). "
                    "controller_decide requires operation UUID, decision and scope. "
                    "controller_state returns bounded domain metadata; controller_hash "
                    "reads one "
                    "completed incoming fixture (at most 1 MiB) and returns SHA-256 "
                    "only. "
                    "controller_online starts normal Tor services; controller_shutdown "
                    "drains storage. "
                    "No caller-specified paths or database queries. Hash reads are "
                    "serialized.";
                descriptor.annotations = {{"readOnlyHint", tool.name == "controller_state" ||
                                                               tool.name == "controller_pending" ||
                                                               tool.name == "controller_hash"},
                                          {"openWorldHint", tool.name == "controller_online"}};
            }
#endif
            const auto name = tool.name;
            descriptor.invoke = [this,
                                 name](json::object args,
                                       yahat::ToolContext context) -> net::awaitable<yahat::ToolResult> {
                co_return co_await call(name, std::move(args), std::move(context));
            };
            mcp->add(std::move(descriptor));
        }
        yahat::RouteOptions route;
        route.allowed_origins = std::vector<std::string>{};
        route.request_filter = [this](const yahat::Request& req) -> std::optional<yahat::Response> {
            service.requestActivity();
            // Reject unauthenticated traffic before JSON parsing. Authenticated
            // HTTP admission has separate control capacity below.
            const auto key = authenticate(std::string(req.getHeader("Authorization")));
            if (key.isEmpty()) {
                std::lock_guard lock(rate_mutex);
                const auto now = std::chrono::steady_clock::now();
                tokens = std::min(20., tokens + std::chrono::duration<double>(now - rate_tick).count() * 2.);
                rate_tick = now;
                if (tokens < 1)
                    return yahat::Response{429, "RateLimited"};
                tokens -= 1;
                return yahat::Response{401, "Unauthorized"};
            }
            if (!req.all_arguments.empty())
                return yahat::Response{400, "QueryArgumentsForbidden"};
            if (!req.getHeader("Content-Encoding").empty())
                return yahat::Response{415, "CompressionUnsupported"};
            boost::system::error_code ec;
            json::parse_options parse_options;
            parse_options.max_depth = 16;
            const auto value = json::parse(req.body, ec, {}, parse_options);
            if (ec || !boundedJson(value))
                return yahat::Response{400, "InvalidInput"};
            bool control = false;
            if (value.is_object()) {
                const auto* method = value.as_object().if_contains("method");
                const auto* params = value.as_object().if_contains("params");
                if (method && *method == "tools/call" && params && params->is_object()) {
                    const auto* name = params->as_object().if_contains("name");
                    control = name && (*name == "operation_status" || *name == "controller_decide");
                }
            }
            {
                std::lock_guard lock(rate_mutex);
                const auto now = std::chrono::steady_clock::now();
                // Credentials are bounded at 64; no untrusted names can create
                // an unbounded rate-limit table.
                if (!principal_budgets.contains(key) && principal_budgets.size() >= 64)
                    return yahat::Response{429, "RateLimited"};
                auto& budget = principal_budgets[key];
                const auto elapsed = std::chrono::duration<double>(now - budget.tick).count();
                budget.ordinary = std::min(100., budget.ordinary + elapsed * 100. / 60.);
                budget.control = std::min(20., budget.control + elapsed * 20. / 60.);
                budget.tick = now;
                // Global replenishment has its own clock, independent of failed auth.
                const auto global_elapsed = std::chrono::duration<double>(now - admission_tick).count();
                ordinary_tokens = std::min(540., ordinary_tokens + global_elapsed * 9.);
                control_tokens = std::min(60., control_tokens + global_elapsed);
                admission_tick = now;
                auto& principal = control ? budget.control : budget.ordinary;
                auto& global = control ? control_tokens : ordinary_tokens;
                if (principal < 1 || global < 1)
                    return yahat::Response{429, "RateLimited"};
                principal -= 1;
                global -= 1;
            }
            return {};
        };
        mcp->mount(*http, "/mcp", std::move(route));
    }
    net::awaitable<yahat::ToolResult> call(QString name, json::object args, yahat::ToolContext context) {
        context.throwIfCancelled();
        auto reply = std::make_shared<Reply>();
        if (const auto* limit = args.if_contains("limit"); limit && !limit->is_int64() && !limit->is_uint64())
            throw yahat::ToolError{"InvalidArguments"};
        const auto wire = json::serialize(args);
        const auto object = QJsonDocument::fromJson(QByteArray(wire.data(), wire.size())).object();
#ifdef DARKSPEAK_WITH_TEST_MCP
        if (controller) {
            QMetaObject::invokeMethod(
                controller, [control = controller, name, object, reply] { control->submit(name, object, reply); },
                Qt::QueuedConnection);
        } else
#endif
        {
            if (!service.reserveDispatch())
                throw yahat::ToolError{"Overloaded"};
            QMetaObject::invokeMethod(
                &service,
                [owner = &service, key = QString::fromStdString(context.auth().account), name, object,
                 reply] { owner->submit(key, name, object, reply); },
                Qt::QueuedConnection);
        }
        net::steady_timer timer(co_await net::this_coro::executor);
        for (;;) {
            // Only the transport wait is cancelled. The Qt-owned admitted
            // operation and immutable approved arguments have independent life.
            context.throwIfCancelled();
            {
                std::lock_guard lock(reply->mutex);
                if (reply->ready) {
                    if (!reply->error.isEmpty())
                        throw yahat::ToolError{reply->error.toStdString()};
                    const auto data = QJsonDocument(reply->value).toJson(QJsonDocument::Compact);
                    auto result = yahat::ToolResult::fromValue(
                        json::parse(std::string_view(data.constData(), data.size())));
                    const auto state = reply->value.value("state").toString();
                    if (name != "operation_status" &&
                        QStringList{"Denied", "Expired", "Cancelled", "Failed", "IndeterminateOperation"}
                            .contains(state))
                        result.is_error = true;
                    co_return result;
                }
            }
            timer.expires_after(std::chrono::milliseconds(20));
            co_await timer.async_wait(net::use_awaitable);
        }
    }
    ~Impl() {
        if (started)
            http->stop();
    }
};
Transport::Transport(Service& service, const QString& address, int port
#ifdef DARKSPEAK_WITH_TEST_MCP
                     ,
                     TestSupport* controller
#endif
                     )
    : impl_(std::make_unique<Impl>(service, address, port
#ifdef DARKSPEAK_WITH_TEST_MCP
                                   ,
                                   controller
#endif
                                   )) {
}
Transport::~Transport() = default;
void Transport::start() {
    impl_->http->start();
    impl_->started = true;
}
} // namespace ds::mcp
