// NetworkBinaryWriter / NetworkBinaryReader payloads (Upstream.Net): big-endian integers, strings as
// i16 BE byte count + UTF-8, Guid as its 16 .NET bytes.
#pragma once

#include "bytes.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>

namespace od
{
struct net_writer
{
	bytes b;
	template <typename T>
	net_writer & be(T v)
	{
		for (int i = int(sizeof(T)) - 1; i >= 0; --i)
			b.push_back(uint8_t(uint64_t(v) >> (8 * i)));
		return *this;
	}
	net_writer & u8(uint8_t v) { b.push_back(v); return *this; }
	net_writer & i16(int16_t v) { return be(v); }
	net_writer & i32(int32_t v) { return be(v); }
	net_writer & i64(int64_t v) { return be(v); }
	net_writer & str(const std::string & s)
	{
		i16(int16_t(std::min<size_t>(s.size(), 32767)));
		b.insert(b.end(), s.begin(), s.begin() + ptrdiff_t(std::min<size_t>(s.size(), 32767)));
		return *this;
	}
	net_writer & raw(const uint8_t * p, size_t n) { b.insert(b.end(), p, p + n); return *this; }
	net_writer & guid(const std::array<uint8_t, 16> & g) { return raw(g.data(), g.size()); }
};

class net_reader
{
public:
	net_reader(const bytes & data) : p(data.data()), end(data.data() + data.size()) {}
	bool ok() const { return good; }
	template <typename T>
	T be()
	{
		if (end - p < ptrdiff_t(sizeof(T)))
			return good = false, T{};
		uint64_t v = 0;
		for (size_t i = 0; i < sizeof(T); ++i)
			v = (v << 8) | *p++;
		return T(v);
	}
	uint8_t u8() { return be<uint8_t>(); }
	int16_t i16() { return be<int16_t>(); }
	int32_t i32() { return be<int32_t>(); }
	int64_t i64() { return be<int64_t>(); }
	std::string str()
	{
		int n = i16();
		if (n < 0 || end - p < n)
			return good = false, std::string();
		std::string s(reinterpret_cast<const char *>(p), size_t(n));
		p += n;
		return s;
	}
	std::array<uint8_t, 16> guid()
	{
		std::array<uint8_t, 16> g{};
		if (end - p < 16)
			return good = false, g;
		std::memcpy(g.data(), p, 16);
		p += 16;
		return g;
	}

private:
	const uint8_t * p;
	const uint8_t * end;
	bool good = true;
};
} // namespace od
