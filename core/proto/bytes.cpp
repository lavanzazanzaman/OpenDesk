#include "bytes.h"

namespace od
{
std::string to_hex(const uint8_t * data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		s += digits[data[i] >> 4];
		s += digits[data[i] & 15];
	}
	return s;
}

bytes from_hex(std::string_view hex)
{
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	bytes out;
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2)
	{
		int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
		if (hi < 0 || lo < 0)
			return {};
		out.push_back(uint8_t(hi << 4 | lo));
	}
	return out;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t * data, size_t size)
{
	std::string out;
	out.reserve((size + 2) / 3 * 4);
	for (size_t i = 0; i < size; i += 3)
	{
		uint32_t n = uint32_t(data[i]) << 16;
		if (i + 1 < size)
			n |= uint32_t(data[i + 1]) << 8;
		if (i + 2 < size)
			n |= data[i + 2];
		out += b64chars[(n >> 18) & 63];
		out += b64chars[(n >> 12) & 63];
		out += i + 1 < size ? b64chars[(n >> 6) & 63] : '=';
		out += i + 2 < size ? b64chars[n & 63] : '=';
	}
	return out;
}

bytes base64_decode(std::string_view b64)
{
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '+')
			return 62;
		if (c == '/')
			return 63;
		return -1;
	};
	bytes out;
	uint32_t acc = 0;
	int bits = 0;
	for (char c: b64)
	{
		int v = value(c);
		if (v < 0)
			continue; // padding / whitespace
		acc = acc << 6 | uint32_t(v);
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out.push_back(uint8_t(acc >> bits));
		}
	}
	return out;
}
} // namespace od
