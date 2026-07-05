#pragma once

#include <cstdint>

namespace sysmon {

class MdnsAdvertiser {
public:
	explicit MdnsAdvertiser(std::uint16_t port);
	~MdnsAdvertiser();

	MdnsAdvertiser(const MdnsAdvertiser&) = delete;
	MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

	bool start();
	void stop() noexcept;

private:
	struct Impl;
	Impl* _impl;
};

} // namespace sysmon
