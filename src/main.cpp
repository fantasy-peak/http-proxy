#include <iostream>

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <gflags/gflags.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <boost/algorithm/string.hpp>

#include "utils.h"

DEFINE_string(host, "0.0.0.0", "http proxy host");
DEFINE_string(log, "./http-proxy.log", "log path");
DEFINE_int32(port, 8849, "http proxy port");
DEFINE_int32(timeout, 5000, "connect timeout ms");
DEFINE_int32(threads, std::thread::hardware_concurrency(), "work threads");
DEFINE_bool(open_file_log, false, "open file log");
DEFINE_int32(log_level, 1, "default info level");

const char NameValueSeparator[] = {':', ' '};
const char Crlf[] = {'\r', '\n'};

constexpr char BadRequest[] =
	"<html>"
	"<head><title>Bad Request</title></head>"
	"<body><h1>400 Bad Request</h1></body>"
	"</html>";

struct Reply {
	struct Header {
		std::string name;
		std::string value;
	};

	std::string status;
	std::vector<Header> headers;
	std::string content;

	std::vector<boost::asio::const_buffer> to_buffers() {
		std::vector<boost::asio::const_buffer> buffers;
		buffers.push_back(boost::asio::buffer(status));
		for (std::size_t i = 0; i < headers.size(); ++i) {
			auto& h = headers[i];
			buffers.push_back(boost::asio::buffer(h.name));
			buffers.push_back(boost::asio::buffer(NameValueSeparator));
			buffers.push_back(boost::asio::buffer(h.value));
			buffers.push_back(boost::asio::buffer(Crlf));
		}
		buffers.push_back(boost::asio::buffer(Crlf));
		buffers.push_back(boost::asio::buffer(content));
		return buffers;
	}
};

Reply reply_bad_request(const std::string& version) {
	Reply rep;
	rep.status = fmt::format("{} 400 Bad Request\r\n", version);
	rep.content = BadRequest;
	rep.headers.resize(2);
	rep.headers[0].name = "Content-Length";
	rep.headers[0].value = std::to_string(rep.content.size());
	rep.headers[1].name = "Content-Type";
	rep.headers[1].value = "text/html";
	return rep;
}

// curl https://www.baidu.com --proxy http://127.0.0.1:8848 --insecure -v -d "a=f"
// curl http://www.baidu.com --proxy http://127.0.0.1:8848 -d "a=f"

template <typename... Args>
void close(Args... args) {
	auto func = [](auto& sock_ptr) {
		if (!sock_ptr->is_open())
			return;
		boost::system::error_code ec;
		sock_ptr->cancel(ec);
		sock_ptr->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
		sock_ptr->close(ec);
	};
	(func(args), ...);
}

std::vector<std::string> split(const std::string& s, const std::string& delimiter) {
	size_t pos_start = 0, pos_end, delim_len = delimiter.length();
	std::vector<std::string> res;
	while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
		auto token = s.substr(pos_start, pos_end - pos_start);
		pos_start = pos_end + delim_len;
		res.emplace_back(std::move(token));
	}
	res.emplace_back(s.substr(pos_start));
	return res;
}

template <typename... Args>
void set_option(Args... args) {
	auto func = [](auto& sock_ptr) {
		boost::system::error_code ec;
		sock_ptr->set_option(boost::asio::ip::tcp::no_delay(true), ec);
		sock_ptr->set_option(boost::asio::socket_base::keep_alive(true), ec);
	};
	(func(args), ...);
}

struct HttpRequestPacket {
	HttpRequestPacket(std::string& req_data)
		: req_data(req_data) {
	}

	bool parse() {
		auto i0 = req_data.find("\r\n");
		std::string req_line = req_data.substr(0, i0);
		spdlog::debug("req_line:  [{}]", req_line);
		std::vector<std::string> method_uri_http_version;
		boost::algorithm::split(method_uri_http_version, req_line, boost::is_any_of(" "), boost::token_compress_on);
		if (method_uri_http_version.size() != 3)
			return false;
		method = std::move(method_uri_http_version.at(0));
		req_uri = std::move(method_uri_http_version.at(1));
		version = std::move(method_uri_http_version.at(2));
		spdlog::debug("method:  [{}]", method);
		spdlog::debug("req_uri: [{}]", req_uri);
		spdlog::debug("version: [{}]", version);
		auto i1 = req_data.find("\r\n\r\n");
		auto req_header = req_data.substr(i0 + 2, i1 - i0 - 2);
		spdlog::debug("req_header: [{}]", req_header);
		for (auto header_lines = split(req_header, "\r\n"); auto& header_line : header_lines) {
			std::vector<std::string> tmp = split(header_line, ": ");
			if (tmp.size() != 2)
				return false;
			headers.emplace(std::move(tmp.at(0)), std::move(tmp.at(1)));
		}
		host = headers.at("Host");
		spdlog::debug("host: [{}]", host);
		req_data = req_data.substr(i1 + 4);
		spdlog::debug("req_data: [{}]", req_data);
		return true;
	}

	std::string method;
	std::string req_uri;
	std::string version;
	std::string req_header;
	std::string host;
	std::string req_data;
	std::unordered_map<std::string, std::string> headers;
};

folly::coro::Task<void> forward(const std::shared_ptr<boost::asio::ip::tcp::socket>& read_sock_ptr,
	const std::shared_ptr<boost::asio::ip::tcp::socket>& write_socket_ptr, const std::string flow_to) {
	constexpr int32_t BufferLen{8192};
	std::unique_ptr<uint8_t[]> buffer_ptr = std::make_unique<uint8_t[]>(BufferLen);
	constexpr int32_t count = BufferLen - 1;
	while (true) {
		auto [ec, bytes_transferred] = co_await async_read_some(*read_sock_ptr, boost::asio::buffer(buffer_ptr.get(), count));
		if (ec) {
			close(write_socket_ptr);
			spdlog::warn("[forward] async_read_some [{}]: {}", flow_to, ec.message());
			co_return;
		}
		if (auto [ec, _] = co_await async_write(*write_socket_ptr, boost::asio::buffer(buffer_ptr.get(), bytes_transferred)); ec) {
			close(read_sock_ptr);
			spdlog::warn("[forward] async_write [{}]: {}", flow_to, ec.message());
			co_return;
		}
		std::memset(buffer_ptr.get(), 0x00, bytes_transferred);
	}
	co_return;
}

folly::coro::Task<void> session(boost::asio::ip::tcp::socket sock, std::shared_ptr<Executor> executor_ptr) {
	auto server_socket_ptr = std::make_shared<boost::asio::ip::tcp::socket>(executor_ptr->m_io_context);
	auto client_socket_ptr = std::make_shared<boost::asio::ip::tcp::socket>(std::move(sock));
	set_option(server_socket_ptr, client_socket_ptr);

	std::string http_header_str;
	auto dynamic_string_buffer = boost::asio::dynamic_string_buffer(http_header_str, 8192);
	if (auto [ec, _] = co_await async_read_until(*client_socket_ptr, dynamic_string_buffer, "\r\n\r\n"); ec) {
		spdlog::error("[session] read http head: {}", ec.message());
		close(client_socket_ptr);
		co_return;
	}

	spdlog::debug("{}", http_header_str);

	auto http_packet = HttpRequestPacket(http_header_str);

	auto reply_to_client = [&]<typename... Ts>(Ts&&... ts) -> folly::coro::Task<> {
		auto reply_buffers = reply_bad_request(http_packet.version);
		co_await async_write(*client_socket_ptr, reply_buffers.to_buffers());
		if constexpr (sizeof...(Ts) != 0)
			close(std::forward<Ts>(ts)...);
	};

	if (auto ret = http_packet.parse(); !ret) {
		spdlog::error("[session] parse error");
		co_await reply_to_client(client_socket_ptr);
		co_return;
	}

	std::string server_host, server_port;
	if (http_packet.host.find(":") != std::string::npos) {
		std::vector<std::string> tmp;
		boost::algorithm::split(tmp, http_packet.host, boost::is_any_of(":"), boost::token_compress_on);
		server_host = std::move(tmp.at(0));
		server_port = std::move(tmp.at(1));
	}
	else {
		server_host = http_packet.host;
		server_port = "80";
	}
	spdlog::debug("server_host: [{}], server_port: [{}]", server_host, server_port);
	auto split_uri_ret = split(http_packet.req_uri, "//");
	auto str = fmt::format("{}//{}", split_uri_ret.at(0), http_packet.host);
	boost::algorithm::replace_first(http_header_str, str, "");

	boost::asio::ip::tcp::resolver resolver{executor_ptr->m_io_context};
	auto [resolver_ec, resolver_results] = co_await async_resolve(resolver, server_host, server_port);
	if (resolver_ec) {
		spdlog::error("async_resolve: {} host: [{}] port: [{}]", resolver_ec.message(), server_host, server_port);
		co_await reply_to_client(client_socket_ptr);
		co_return;
	}
	spdlog::debug("resolver_results size: [{}]", resolver_results.size());
	for (auto& endpoint : resolver_results) {
		std::stringstream ss;
		ss << endpoint.endpoint();
		spdlog::debug("resolver_results: [{}]", ss.str());
	}

	spdlog::debug("async_connect: [{}:{}]", server_host, server_port);
	if (auto ec = co_await async_connect(executor_ptr->m_io_context, *server_socket_ptr, resolver_results, FLAGS_timeout); ec) {
		spdlog::error("async_connect: {}, host: [{}] port: [{}]", ec.message(), server_host, server_port);
		co_await reply_to_client(client_socket_ptr, server_socket_ptr);
		co_return;
	}
	spdlog::debug("Connected: [{}:{}]", server_host, server_port);

	static std::unordered_set<std::string> methods{"GET", "POST", "PUT", "DELETE", "HEAD"};
	if (methods.contains(http_packet.method)) {
		if (auto [ec, _] = co_await async_write(*server_socket_ptr, boost::asio::buffer(http_header_str, http_header_str.size()));
			ec) {
			spdlog::error("[session]: forward data to [{}:{}] {}", server_host, server_port, ec.message());
			co_await reply_to_client(client_socket_ptr, server_socket_ptr);
			co_return;
		}
	}
	else if (http_packet.method == "CONNECT") {
		auto success_msg = fmt::format("{} 200 Connection Established\r\nConnection: close\r\n\r\n", http_packet.version);
		if (auto [ec, _] = co_await async_write(*client_socket_ptr, boost::asio::buffer(success_msg, success_msg.size())); ec) {
			spdlog::error("[session] write connection_established {}", ec.message());
			co_await reply_to_client(client_socket_ptr, server_socket_ptr);
			co_return;
		}
	}
	else {
		spdlog::error("[session] not support {}", http_packet.method);
		co_await reply_to_client(client_socket_ptr, server_socket_ptr);
		co_return;
	}

	// clang-format off
	folly::coro::co_invoke([executor_ptr, client_socket_ptr, server_socket_ptr]() -> folly::coro::Task<> {
		co_await forward(client_socket_ptr, server_socket_ptr, "client-to-server");
	}).scheduleOn(executor_ptr.get()).start();
	folly::coro::co_invoke([executor_ptr, client_socket_ptr, server_socket_ptr]() -> folly::coro::Task<> {
		co_await forward(server_socket_ptr, client_socket_ptr, "server-to-client");
	}).scheduleOn(executor_ptr.get()).start();
	// clang-format on

	co_return;
}

int main(int argc, char** argv) {
	try {
		gflags ::SetVersionString("0.0.0.1");
		gflags ::SetUsageMessage("Usage : ./http-proxy");
		google::ParseCommandLineFlags(&argc, &argv, true);
		if (FLAGS_open_file_log) {
			auto logger = spdlog::basic_logger_mt("HTTP-PROXY", FLAGS_log);
			logger->flush_on(spdlog::level::debug);
			spdlog::set_default_logger(logger);
			if (FLAGS_log_level == 0)
				spdlog::set_level(spdlog::level::debug);
			// spdlog::flush_every(std::chrono::seconds(1));
		}
		if (FLAGS_log_level == 0)
			spdlog::set_level(spdlog::level::debug);
		else
			spdlog::set_level(spdlog::level::info);
		IoContextPool pool(FLAGS_threads);
		std::jthread thd([&] { pool.start(); });
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		auto& context = pool.getIoContext();
		boost::asio::ip::tcp::acceptor acceptor(context);
		boost::asio::ip::tcp::resolver resolver(context);
		boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(FLAGS_host, std::to_string(FLAGS_port)).begin();
		std::stringstream ss;
		ss << endpoint;
		spdlog::info("start accept at {} ...", ss.str());
		acceptor.open(endpoint.protocol());
		acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
		acceptor.bind(endpoint);
		boost::system::error_code ec;
		acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
		if (ec) {
			spdlog::error("{}", ec.message());
			return -1;
		}
		boost::asio::signal_set sigset(context, SIGINT, SIGTERM);
		sigset.async_wait([&](const boost::system::error_code&, int) { acceptor.close(); });
		auto make_accept_task = [&]() -> folly::coro::Task<void> {
			while (true) {
				auto& context = pool.getIoContext();
				boost::asio::ip::tcp::socket socket(context);
				auto ec = co_await async_accept(acceptor, socket);
				if (ec) {
					if (ec == boost::asio::error::operation_aborted)
						break;
					spdlog::error("Accept failed, error: {}", ec.message());
					continue;
				}
				auto executor = std::make_shared<Executor>(context);
				auto make_session_task = [executor, socket = std::move(socket)]() mutable -> folly::coro::Task<void> {
					try {
						co_await session(std::move(socket), std::move(executor));
					} catch (const std::exception& e) {
						spdlog::error("session error: {}", e.what());
					}
					co_return;
				};
				folly::coro::co_invoke(std::move(make_session_task)).scheduleOn(executor.get()).start();
			}
		};
		folly::coro::blockingWait(make_accept_task());
		pool.stop();
	} catch (std::exception& e) {
		spdlog::error("Exception: {}", e.what());
	}
	return 0;
}