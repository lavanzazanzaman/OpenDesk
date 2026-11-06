// upstream's AesStream framing and the discovery crypto (Upstream.Net.AesStream / ComputerDiscoveryClient).
//
// AesStream: AES-256-CBC, PaddingMode.None, one CBC chain per direction for the whole connection.
// Every Write() becomes one chunk: 7-bit varint length + payload + random padding to a 16-byte multiple.
#pragma once

#include "bytes.h"

#include <array>
#include <memory>
#include <optional>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace od
{
using aes_key = std::array<uint8_t, 32>;
using aes_iv = std::array<uint8_t, 16>;

void random_bytes(uint8_t * out, size_t size);

template <size_t N>
std::array<uint8_t, N> random_array()
{
	std::array<uint8_t, N> a;
	random_bytes(a.data(), N);
	return a;
}

class aes_cbc
{
public:
	aes_cbc(const aes_key &, const aes_iv &, bool encrypt);
	~aes_cbc();
	aes_cbc(const aes_cbc &) = delete;
	aes_cbc & operator=(const aes_cbc &) = delete;
	aes_cbc(aes_cbc &&) noexcept;

	// size must be a multiple of 16; in == out allowed
	void process(const uint8_t * in, uint8_t * out, size_t size);

private:
	EVP_CIPHER_CTX * ctx;
};

void append_varint(bytes & out, uint32_t value);
// returns {value, bytes consumed}; nullopt if malformed
std::optional<std::pair<uint32_t, size_t>> read_varint(const uint8_t * p, size_t size);

class aes_stream_writer
{
public:
	aes_stream_writer(const aes_key & key, const aes_iv & iv) :
	        cipher(key, iv, true) {}
	// one AesStream.Write(): returns the ciphertext chunk
	bytes encode(const uint8_t * payload, size_t size);
	bytes encode(const bytes & payload) { return encode(payload.data(), payload.size()); }

private:
	aes_cbc cipher;
};

// Incremental reader for a TCP AesStream: feed ciphertext as it arrives, take plaintext out.
class aes_stream_reader
{
public:
	aes_stream_reader(const aes_key & key, const aes_iv & iv) :
	        cipher(key, iv, false) {}
	void feed(const uint8_t * data, size_t size);
	bytes & plaintext() { return plain; } // caller consumes from the front
	bool error() const { return failed; }

private:
	aes_cbc cipher;
	bytes pending;         // ciphertext not yet decrypted
	bytes chunk;           // decrypted head of the current chunk
	size_t chunk_total = 0; // total (padded) size of the current chunk, 0 = unknown
	bytes plain;
	bool failed = false;
};

// Whole-datagram decode (UDP discovery): returns the chunks' payloads
std::optional<std::vector<bytes>> aes_stream_decode_chunks(const aes_key &, const aes_iv &, const uint8_t * data, size_t size);

// RSA-OAEP(SHA-1) encryption with a .NET RSAParameters XML public key (<RSAKeyValue><Modulus>..<Exponent>..)
std::optional<bytes> rsa_oaep_sha1_encrypt_xml(std::string_view rsa_xml, const uint8_t * data, size_t size);
} // namespace od
