#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace sysmon {

class NetworkServer {
public:
	using LineProvider = std::function<std::string()>;
	using ListeningCallback = std::function<void()>;
	using ClientCallback = std::function<void()>;

	NetworkServer(std::uint16_t port, bool allowRemoteClients, LineProvider provider, ListeningCallback onListening = {}, ClientCallback onClientDisconnected = {});
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
