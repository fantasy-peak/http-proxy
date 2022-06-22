#pragma once

#include <list>
#include <memory>
#include <thread>

#include <folly/experimental/coro/Task.h>
#include <boost/asio.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

class Executor : public folly::Executor {
public:
	Executor(boost::asio::io_context& io_context)
		: m_io_context(io_context) {
	}

	virtual void add(folly::Func func) override {
		boost::asio::post(m_io_context, std::move(func));
	}

	boost::asio::io_context& m_io_context;
};

class IoContextPool final {
public:
	explicit IoContextPool(std::size_t);

	void start();
	void stop();

	boost::asio::io_context& getIoContext();

private:
	std::vector<std::shared_ptr<boost::asio::io_context>> m_io_contexts;
	std::list<boost::asio::any_io_executor> m_work;
	std::size_t m_next_io_context;
	std::vector<std::jthread> m_threads;
};

inline IoContextPool::IoContextPool(std::size_t pool_size)
	: m_next_io_context(0) {
	if (pool_size == 0)
		throw std::runtime_error("IoContextPool size is 0");
	for (std::size_t i = 0; i < pool_size; ++i) {
		auto io_context_ptr = std::make_shared<boost::asio::io_context>();
		m_io_contexts.emplace_back(io_context_ptr);
		m_work.emplace_back(boost::asio::require(io_context_ptr->get_executor(), boost::asio::execution::outstanding_work.tracked));
	}
}

inline void IoContextPool::start() {
	for (auto& context : m_io_contexts)
		m_threads.emplace_back(std::jthread([&] { context->run(); }));
}

inline void IoContextPool::stop() {
	for (auto& context_ptr : m_io_contexts)
		context_ptr->stop();
}

inline boost::asio::io_context& IoContextPool::getIoContext() {
	boost::asio::io_context& io_context = *m_io_contexts[m_next_io_context];
	++m_next_io_context;
	if (m_next_io_context == m_io_contexts.size())
		m_next_io_context = 0;
	return io_context;
}

template <typename Body, typename Allocator>
class AcceptorAwaiterWs {
public:
	AcceptorAwaiterWs(boost::beast::websocket::stream<boost::beast::tcp_stream>& stream, boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> req)
		: m_ws_stream(stream)
		, m_req(std::move(req)) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_ws_stream.async_accept(m_req, [this, handle](boost::beast::error_code ec) {
			m_ec = std::move(ec);
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	boost::beast::websocket::stream<boost::beast::tcp_stream>& m_ws_stream;
	boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> m_req;
	boost::beast::error_code m_ec{};
};

class AcceptorAwaiter {
public:
	AcceptorAwaiter(boost::asio::ip::tcp::acceptor& acceptor, boost::asio::ip::tcp::socket& socket)
		: m_acceptor(acceptor)
		, m_socket(socket) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		m_acceptor.async_accept(m_socket, [this, handle](auto ec) mutable {
			m_ec = ec;
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	boost::asio::ip::tcp::acceptor& m_acceptor;
	boost::asio::ip::tcp::socket& m_socket;
	boost::system::error_code m_ec{};
};

inline folly::coro::Task<boost::system::error_code> async_accept(boost::asio::ip::tcp::acceptor& acceptor, boost::asio::ip::tcp::socket& socket) noexcept {
	co_return co_await AcceptorAwaiter{acceptor, socket};
}

template <typename Body, typename Allocator>
inline folly::coro::Task<boost::beast::error_code> async_accept_ws(
	boost::beast::websocket::stream<boost::beast::tcp_stream>& ws_stream,
	boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> req) noexcept {
	co_return co_await AcceptorAwaiterWs{ws_stream, std::move(req)};
}

template <typename Socket, typename AsioBuffer>
struct ReadAwaiterWs {
public:
	ReadAwaiterWs(Socket& socket, AsioBuffer& buffer)
		: m_socket(socket)
		, m_buffer(buffer) {}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_read(m_buffer, [this, handle](auto ec, auto size) mutable {
			m_ec = std::move(ec);
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::beast::error_code, size_t>>
async_read_ws(Socket& socket, AsioBuffer& buffer) noexcept {
	co_return co_await ReadAwaiterWs{socket, buffer};
}

template <typename Socket, typename AsioBuffer, typename Parser>
struct ReadAwaiterHttp {
public:
	ReadAwaiterHttp(Socket& socket, AsioBuffer& buffer, Parser& parser)
		: m_socket(socket)
		, m_buffer(buffer)
		, m_parser(parser) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		boost::beast::http::async_read(m_socket, m_buffer, m_parser, [this, handle](boost::beast::error_code ec, std::size_t bytes_transferred) {
			m_ec = std::move(ec);
			size_ = bytes_transferred;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;
	Parser& m_parser;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer, typename Parser>
inline folly::coro::Task<std::pair<boost::beast::error_code, size_t>> async_read_http(Socket& socket, AsioBuffer& buffer, Parser& parser) noexcept {
	co_return co_await ReadAwaiterHttp{socket, buffer, parser};
}

template <typename Socket, typename AsioBuffer>
struct WriteAwaiterWs {
public:
	WriteAwaiterWs(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(std::move(buffer)) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_write(m_buffer, [this, handle](boost::beast::error_code ec, std::size_t size) {
			m_ec = std::move(ec);
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::system::error_code, size_t>> async_write_ws(Socket& socket, AsioBuffer&& buffer) noexcept {
	co_return co_await WriteAwaiterWs{socket, std::move(buffer)};
}

template <typename Socket, typename AsioBuffer>
struct ReadSomeAwaiter {
public:
	ReadSomeAwaiter(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(buffer) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		m_socket.async_read_some(std::move(m_buffer), [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::system::error_code, size_t>> async_read_some(Socket& socket, AsioBuffer&& buffer) noexcept {
	co_return co_await ReadSomeAwaiter{socket, std::move(buffer)};
}

template <typename Socket, typename AsioBuffer>
struct ReadAwaiter {
public:
	ReadAwaiter(Socket& socket, AsioBuffer& buffer)
		: m_socket(socket)
		, m_buffer(buffer) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		boost::asio::async_read(m_socket, m_buffer, [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::system::error_code, size_t>> async_read(Socket& socket, AsioBuffer& buffer) noexcept {
	co_return co_await ReadAwaiter{socket, buffer};
}

template <typename Socket, typename AsioBuffer>
struct ReadUntilAwaiter {
public:
	ReadUntilAwaiter(Socket& socket, AsioBuffer& buffer, boost::asio::string_view delim)
		: m_socket(socket)
		, m_buffer(buffer)
		, delim_(delim) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		boost::asio::async_read_until(m_socket, m_buffer, delim_, [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer& m_buffer;
	boost::asio::string_view delim_;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::system::error_code, size_t>> async_read_until(Socket& socket, AsioBuffer& buffer,
	boost::asio::string_view delim) noexcept {
	co_return co_await ReadUntilAwaiter{socket, buffer, delim};
}

template <typename Socket, typename AsioBuffer>
struct WriteAwaiter {
public:
	WriteAwaiter(Socket& socket, AsioBuffer&& buffer)
		: m_socket(socket)
		, m_buffer(std::move(buffer)) {
	}

	bool await_ready() { return false; }
	auto await_resume() { return std::make_pair(m_ec, size_); }
	void await_suspend(std::coroutine_handle<> handle) {
		boost::asio::async_write(m_socket, std::move(m_buffer), [this, handle](auto ec, auto size) mutable {
			m_ec = ec;
			size_ = size;
			handle.resume();
		});
	}

private:
	Socket& m_socket;
	AsioBuffer m_buffer;

	boost::system::error_code m_ec{};
	size_t size_{0};
};

template <typename Socket, typename AsioBuffer>
inline folly::coro::Task<std::pair<boost::system::error_code, size_t>> async_write(Socket& socket, AsioBuffer&& buffer) noexcept {
	co_return co_await WriteAwaiter{socket, std::move(buffer)};
}

class ConnectAwaiter {
public:
	ConnectAwaiter(boost::asio::io_context& io_context, boost::asio::ip::tcp::socket& socket,
		boost::asio::ip::tcp::resolver::results_type& results_type, int32_t timeout)
		: io_context_(io_context)
		, m_socket(socket)
		, m_results_type(results_type)
		, m_steady_timer(io_context)
		, m_timeout(timeout) {
	}

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) {
		auto done = std::make_shared<bool>(false);
		m_steady_timer.expires_after(std::chrono::milliseconds(m_timeout));
		m_steady_timer.async_wait([this, handle, done](const boost::system::error_code& ec) {
			if (*done)
				return;
			*done = true;
			m_ec = boost::asio::error::timed_out;
			handle.resume();
		});
		boost::asio::async_connect(m_socket, m_results_type, [this, handle, done](boost::system::error_code ec, auto&&) mutable {
			if (*done)
				return;
			*done = true;
			m_ec = std::move(ec);
			handle.resume();
		});
	}
	auto await_resume() noexcept { return m_ec; }

private:
	boost::asio::io_context& io_context_;
	boost::asio::ip::tcp::socket& m_socket;
	boost::asio::ip::tcp::resolver::results_type& m_results_type;
	boost::asio::steady_timer m_steady_timer;
	int32_t m_timeout;
	boost::system::error_code m_ec{};
};

inline folly::coro::Task<boost::system::error_code> async_connect(boost::asio::io_context& io_context, boost::asio::ip::tcp::socket& socket,
	boost::asio::ip::tcp::resolver::results_type& results_type, int32_t timeout = 5000) noexcept {
	co_return co_await ConnectAwaiter{io_context, socket, results_type, timeout};
}
