#pragma once

// Wrap an external command-line program as a libagent Tool with one call.
//
// Requires Boost.Process v2 (Boost >= 1.86): users of this header must also
// link Boost::process, e.g.
//   find_package(Boost CONFIG REQUIRED COMPONENTS headers process)
//   target_link_libraries(my_app PRIVATE libagent::libagent Boost::process)
// The core libagent library does NOT depend on Boost.Process; this is an opt-in
// convenience header.

#include "libagent/json.hpp"
#include "libagent/tool.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <array>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libagent::cli {

/// Replace every `{name}` in `tmpl` with the value of `args["name"]`. Strings
/// are inserted verbatim; other JSON types are inserted via their JSON spelling.
/// The result becomes a single argv element, so values are passed to the program
/// directly (exec'd, never through a shell) — no shell-injection risk.
inline std::string substitute(std::string_view tmpl, const Json& args) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const auto end = tmpl.find('}', i);
            if (end != std::string_view::npos) {
                const std::string key(tmpl.substr(i + 1, end - i - 1));
                if (args.contains(key)) {
                    const Json& v = args[key];
                    if (v.is_string()) {
                        out += v.get<std::string>();
                    } else {
                        out += v.dump();
                    }
                }
                i = end + 1;
                continue;
            }
        }
        out += tmpl[i++];
    }
    return out;
}

/// Parameter declaration: {name, json_type} where json_type is "string",
/// "number", "integer", or "boolean".
using ParamSpec = std::vector<std::pair<std::string, std::string>>;

/// Read a readable_pipe to EOF into a string.
inline boost::asio::awaitable<std::string> drain(boost::asio::readable_pipe& pipe) {
    std::string data;
    char buf[4096];
    boost::system::error_code ec;
    while (true) {
        const std::size_t n = co_await pipe.async_read_some(
            boost::asio::buffer(buf),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) {
            break;
        }
        data.append(buf, n);
    }
    co_return data;
}

/// Build a Tool that runs `program` with `argv_template`. Each argv entry may
/// contain `{placeholders}` substituted from the call's JSON args. The handler
/// returns:
///   {"exit": <int>, "stdout": <string>, "stderr": <string>}
/// (or {"error": <message>} if the program could not be launched).
///
/// Example:
///   registry->add(cli::command(
///       "weather", "Get weather for a city",
///       {{"city", "string"}},
///       "/usr/bin/curl",
///       {"-s", "https://wttr.in/{city}?format=3"}));
inline Tool command(std::string name, std::string description, ParamSpec params,
                    std::filesystem::path program,
                    std::vector<std::string> argv_template) {
    Tool t;
    t.spec.name = std::move(name);
    t.spec.description = std::move(description);

    Json props = Json::object();
    Json required = Json::array();
    for (const auto& [pname, ptype] : params) {
        props[pname] = Json{{"type", ptype}};
        required.push_back(pname);
    }
    t.spec.parameters =
        Json{{"type", "object"}, {"properties", props}, {"required", required}};

    t.handler = [program = std::move(program),
                 argv_template = std::move(argv_template)](
                    const Json& args) -> boost::asio::awaitable<Json> {
        std::vector<std::string> argv;
        argv.reserve(argv_template.size());
        for (const auto& tmpl : argv_template) {
            argv.push_back(substitute(tmpl, args));
        }

        auto ex = co_await boost::asio::this_coro::executor;
        std::string out_str;
        std::string err_str;
        int exit_code = 0;

        try {
            boost::asio::readable_pipe out_pipe(ex);
            boost::asio::readable_pipe err_pipe(ex);
            boost::process::v2::process proc(
                ex, program.string(), argv,
                boost::process::v2::process_stdio{/*in*/ {}, out_pipe, err_pipe});

            // Drain both pipes concurrently so a full stderr buffer can't
            // deadlock the child.
            using Chan = boost::asio::experimental::channel<void(
                boost::system::error_code, std::size_t)>;
            auto chan = std::make_shared<Chan>(ex, 2u);
            auto results = std::make_shared<std::array<std::string, 2>>();

            for (std::size_t which = 0; which < 2; ++which) {
                boost::asio::co_spawn(
                    ex,
                    [&out_pipe, &err_pipe, which, chan, results]() -> boost::asio::awaitable<void> {
                        boost::asio::awaitable<std::string> reader;
                        if (which == 0) {
                            reader = drain(out_pipe);
                        } else {
                            reader = drain(err_pipe);
                        }
                        (*results)[which] = co_await std::move(reader);
                        co_await chan->async_send(boost::system::error_code{}, which,
                                                  boost::asio::use_awaitable);
                    },
                    boost::asio::detached);
            }
            co_await chan->async_receive(boost::asio::use_awaitable);
            co_await chan->async_receive(boost::asio::use_awaitable);

            out_str = std::move((*results)[0]);
            err_str = std::move((*results)[1]);
            exit_code = co_await proc.async_wait(boost::asio::use_awaitable);
        } catch (const std::exception& e) {
            co_return Json{{"error", e.what()}};
        }

        co_return Json{{"exit", exit_code},
                       {"stdout", std::move(out_str)},
                       {"stderr", std::move(err_str)}};
    };
    return t;
}

}  // namespace libagent::cli
