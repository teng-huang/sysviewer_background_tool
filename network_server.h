#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sysmon {

class NetworkServer {
public:
	using LineProvider = std::function<std::string()>;

	NetworkServer(std::uint16_t port, LineProvider provider);
	~NetworkServer();

	NetworkServer(const NetworkServer&) = delete;
	NetworkServer& operator=(const NetworkServer&) = delete;

	int run();
	void stop() noexcept;

private:
	struct Impl;
	Impl* _impl;
};

} // namespace sysmon
