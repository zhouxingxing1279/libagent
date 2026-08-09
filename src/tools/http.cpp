#include "libagent/tools/http.hpp"

#include "http/https_client.hpp"
#include "libagent/json.hpp"
#include "libagent/types.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>

#include <exception>
#include <string>

namespace libagent::tools {

namespace {

namespace beasthttp = boost::beast::http;

struct Url {
    bool tls = true;
    std::string host;
    std::string port;
    std::string target = "/";
};

Url parse_url(const std::string& url) {
    Url u;
    std::string rest = url;
    if (url.rfind("https://", 0) == 0) {
        u.tls = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        u.tls = false;
        rest = url.substr(7);
    }
    u.port = u.tls ? "443" : "80";

    const auto slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    u.target = (slash == std::string::npos) ? "/" : rest.substr(slash);

    const auto colon = authority.find(':');
    if (colon != std::string::npos) {
        u.host = authority.substr(0, colon);
        u.port = authority.substr(colon + 1);
    } else {
        u.host = authority;
    }
    return u;
}

}  // namespace

Tool http_get(std::string name, std::string description, HttpGetOptions opts) {
    Tool t;
    t.spec.name = std::move(name);
    t.spec.description = std::move(description);
    t.spec.parameters = Json{{"type", "object"},
                             {"properties",
                              Json{{"url",
                                    Json{{"type", "string"}, {"description", "The URL to fetch."}}}}},
                             {"required", Json::array({"url"})}};

    t.handler = [opts](const Json& args) -> boost::asio::awaitable<Json> {
        const std::string url = args.value("url", std::string{});
        try {
            const Url u = parse_url(url);
            http::Request r;
            r.host = u.host;
            r.service = u.port;
            r.target = u.target;
            r.method = beasthttp::verb::get;
            r.use_tls = u.tls;
            r.timeout = opts.timeout;

            http::Response resp = co_await http::HttpsClient{}.request(r);

            std::string body = std::move(resp.body);
            if (opts.max_body_bytes != 0 && body.size() > opts.max_body_bytes) {
                body.resize(opts.max_body_bytes);
                body += "\n...[truncated by libagent]";
            }
            co_return Json{{"status", resp.status}, {"body", std::move(body)}};
        } catch (const std::exception& e) {
            co_return Json{{"error", e.what()}};
        }
    };
    return t;
}

}  // namespace libagent::tools
