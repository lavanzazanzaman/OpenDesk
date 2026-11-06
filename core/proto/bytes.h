// Small byte-buffer helpers shared by the protocol code (no Qt: this layer is reused by the VR pieces).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace od
{
using bytes = std::vector<uint8_t>;

inline void append(bytes & out, const void * data, size_t size)
{
	auto p = static_cast<const uint8_t *>(data);
	out.insert(out.end(), p, p + size);
}

inline void append(bytes & out, std::string_view s)
{
	append(out, s.data(), s.size());
}

template <typename T>
void append_le(bytes & out, T value) // little-endian POD (x86-64 and the headset are both LE)
{
	append(out, &value, sizeof(value));
}

template <typename T>
T read_le(const uint8_t * p)
{
	T v;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

inline void append_be32(bytes & out, int32_t v) // NetworkBinaryWriter.Write(int)
{
	uint8_t b[4] = {uint8_t(uint32_t(v) >> 24), uint8_t(uint32_t(v) >> 16), uint8_t(uint32_t(v) >> 8), uint8_t(v)};
	append(out, b, 4);
}

inline int32_t read_be32(const uint8_t * p)
{
	return int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]));
}

std::string to_hex(const uint8_t * data, size_t size);
inline std::string to_hex(const bytes & b)
{
	return to_hex(b.data(), b.size());
}
bytes from_hex(std::string_view hex);

std::string base64_encode(const uint8_t * data, size_t size);
bytes base64_decode(std::string_view b64);
} // namespace od
