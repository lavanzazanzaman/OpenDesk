// Test CLI exposing the C++ protocol primitives over hex, driven by crosscheck.py against the Python reference.
//   aes-encode KEY IV            stdin: one payload hex per line -> one ciphertext chunk hex per line (one CBC chain)
//   aes-decode KEY IV CHUNKSIZE  stdin: ciphertext hex -> plaintext hex (fed in CHUNKSIZE pieces)
//   parse-request                stdin: datagram hex -> "platform account"
//   discovery-reply              stdin: request hex -> reply hex (fixed test computer)
//   computer-xml                 -> XML of the fixed test computer
#include "protocol.h"

#include <algorithm>
#include <iostream>
#include <string>

using namespace od;

static computer_info test_computer()
{
	computer_info c;
	c.id = "e32d4ce0-ff56-474b-a840-97889932304c";
	c.name = "CachyOS & <test>";
	c.description = "Linux streamer";
	c.adapters = {{"192.168.178.101", "74563CB94066", false, true, false}, {"fe80::1", "", false, true, true}};
	for (int i = 0; i < 16; ++i)
		c.connection_id[i] = uint8_t(i);
	for (int i = 0; i < 32; ++i)
		c.key[i] = uint8_t(0x40 + i);
	for (int i = 0; i < 16; ++i)
		c.iv[i] = uint8_t(0x80 + i);
	return c;
}

template <size_t N>
static std::array<uint8_t, N> arr(const std::string & hex)
{
	std::array<uint8_t, N> a{};
	auto b = from_hex(hex);
	std::copy_n(b.begin(), std::min(N, b.size()), a.begin());
	return a;
}

int main(int argc, char ** argv)
{
	std::string cmd = argc > 1 ? argv[1] : "";
	std::string line;
	if (cmd == "aes-encode")
	{
		aes_stream_writer w(arr<32>(argv[2]), arr<16>(argv[3]));
		while (std::getline(std::cin, line))
			std::cout << to_hex(w.encode(from_hex(line))) << "\n";
	}
	else if (cmd == "aes-decode")
	{
		aes_stream_reader r(arr<32>(argv[2]), arr<16>(argv[3]));
		size_t step = std::stoul(argv[4]);
		std::getline(std::cin, line);
		bytes ct = from_hex(line);
		for (size_t i = 0; i < ct.size(); i += step)
			r.feed(ct.data() + i, std::min(step, ct.size() - i));
		std::cout << (r.error() ? "ERROR" : to_hex(r.plaintext())) << "\n";
	}
	else if (cmd == "parse-request")
	{
		std::getline(std::cin, line);
		bytes d = from_hex(line);
		auto req = parse_discovery_request(d.data(), d.size());
		if (!req)
			return 1;
		std::cout << req->platform << " " << req->account_id << "\n";
	}
	else if (cmd == "discovery-reply")
	{
		std::getline(std::cin, line);
		bytes d = from_hex(line);
		auto req = parse_discovery_request(d.data(), d.size());
		auto reply = req ? build_discovery_reply(*req, test_computer()) : std::nullopt;
		if (!reply)
			return 1;
		std::cout << to_hex(*reply) << "\n";
	}
	else if (cmd == "computer-xml")
		std::cout << test_computer().to_xml() << "\n";
	else
	{
		std::cerr << "unknown command\n";
		return 2;
	}
	return 0;
}
