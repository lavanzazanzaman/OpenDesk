#include "crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <regex>
#include <stdexcept>

namespace od
{
void random_bytes(uint8_t * out, size_t size)
{
	if (RAND_bytes(out, int(size)) != 1)
		throw std::runtime_error("RAND_bytes failed");
}

aes_cbc::aes_cbc(const aes_key & key, const aes_iv & iv, bool encrypt) :
        ctx(EVP_CIPHER_CTX_new())
{
	if (!ctx || EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data(), encrypt ? 1 : 0) != 1)
		throw std::runtime_error("EVP_CipherInit_ex failed");
	EVP_CIPHER_CTX_set_padding(ctx, 0); // PaddingMode.None; the stream carries its own lengths
}

aes_cbc::~aes_cbc()
{
	EVP_CIPHER_CTX_free(ctx);
}

aes_cbc::aes_cbc(aes_cbc && other) noexcept :
        ctx(other.ctx)
{
	other.ctx = nullptr;
}

void aes_cbc::process(const uint8_t * in, uint8_t * out, size_t size)
{
	int len = 0;
	if (size % 16 || EVP_CipherUpdate(ctx, out, &len, in, int(size)) != 1 || size_t(len) != size)
		throw std::runtime_error("AES-CBC update failed");
}

void append_varint(bytes & out, uint32_t value)
{
	while (value >= 0x80)
	{
		out.push_back(uint8_t(value | 0x80));
		value >>= 7;
	}
	out.push_back(uint8_t(value));
}

std::optional<std::pair<uint32_t, size_t>> read_varint(const uint8_t * p, size_t size)
{
	uint32_t value = 0;
	for (size_t i = 0; i < size && i < 5; ++i)
	{
		value |= uint32_t(p[i] & 0x7f) << (7 * i);
		if (!(p[i] & 0x80))
			return std::pair{value, i + 1};
	}
	return std::nullopt;
}

bytes aes_stream_writer::encode(const uint8_t * payload, size_t size)
{
	bytes block;
	block.reserve(size + 21);
	append_varint(block, uint32_t(size));
	append(block, payload, size);
	size_t pad = (16 - block.size() % 16) % 16;
	if (pad)
	{
		uint8_t random[16];
		random_bytes(random, pad);
		append(block, random, pad);
	}
	cipher.process(block.data(), block.data(), block.size());
	return block;
}

void aes_stream_reader::feed(const uint8_t * data, size_t size)
{
	append(pending, data, size);
	size_t pos = 0;
	while (!failed)
	{
		if (chunk_total == 0)
		{
			// need the first block to learn the chunk length
			if (pending.size() - pos < 16)
				break;
			chunk.assign(pending.begin() + pos, pending.begin() + pos + 16);
			cipher.process(chunk.data(), chunk.data(), 16);
			pos += 16;
			auto len = read_varint(chunk.data(), 16);
			if (!len)
			{
				failed = true;
				break;
			}
			size_t header = len->second, payload = len->first;
			chunk_total = (header + payload + 15) / 16 * 16;
		}
		size_t missing = chunk_total - chunk.size();
		size_t available = (pending.size() - pos) / 16 * 16;
		size_t take = std::min(missing, available);
		if (take)
		{
			size_t old = chunk.size();
			chunk.resize(old + take);
			cipher.process(pending.data() + pos, chunk.data() + old, take);
			pos += take;
		}
		if (chunk.size() < chunk_total)
			break;
		auto len = read_varint(chunk.data(), chunk.size());
		plain.insert(plain.end(), chunk.begin() + len->second, chunk.begin() + len->second + len->first);
		chunk.clear();
		chunk_total = 0;
	}
	pending.erase(pending.begin(), pending.begin() + pos);
}

std::optional<std::vector<bytes>> aes_stream_decode_chunks(const aes_key & key, const aes_iv & iv, const uint8_t * data, size_t size)
{
	size_t usable = size / 16 * 16;
	bytes plain(data, data + usable);
	aes_cbc(key, iv, false).process(plain.data(), plain.data(), usable);
	std::vector<bytes> chunks;
	size_t pos = 0;
	while (pos + 16 <= plain.size())
	{
		auto len = read_varint(plain.data() + pos, plain.size() - pos);
		if (!len || pos + len->second + len->first > plain.size())
			return std::nullopt;
		const uint8_t * start = plain.data() + pos + len->second;
		chunks.emplace_back(start, start + len->first);
		pos += (len->second + len->first + 15) / 16 * 16;
	}
	return chunks;
}

std::optional<bytes> rsa_oaep_sha1_encrypt_xml(std::string_view rsa_xml, const uint8_t * data, size_t size)
{
	auto field = [&](const char * tag) -> bytes {
		std::regex re(std::string("<") + tag + ">([^<]+)</" + tag + ">");
		std::match_results<std::string_view::const_iterator> m;
		if (!std::regex_search(rsa_xml.begin(), rsa_xml.end(), m, re))
			return {};
		return base64_decode(std::string_view(&*m[1].first, m[1].length()));
	};
	bytes n = field("Modulus"), e = field("Exponent");
	if (n.empty() || e.empty())
		return std::nullopt;

	std::optional<bytes> result;
	BIGNUM * bn_n = BN_bin2bn(n.data(), int(n.size()), nullptr);
	BIGNUM * bn_e = BN_bin2bn(e.data(), int(e.size()), nullptr);
	OSSL_PARAM_BLD * bld = OSSL_PARAM_BLD_new();
	OSSL_PARAM * params = nullptr;
	EVP_PKEY_CTX * from = nullptr;
	EVP_PKEY * key = nullptr;
	EVP_PKEY_CTX * enc = nullptr;
	do
	{
		if (!bn_n || !bn_e || !bld || !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n) ||
		    !OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e) || !(params = OSSL_PARAM_BLD_to_param(bld)))
			break;
		if (!(from = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)) || EVP_PKEY_fromdata_init(from) <= 0 ||
		    EVP_PKEY_fromdata(from, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0)
			break;
		// RSACryptoServiceProvider.Encrypt(data, fOAEP: true) = OAEP with SHA-1 and MGF1-SHA-1
		if (!(enc = EVP_PKEY_CTX_new(key, nullptr)) || EVP_PKEY_encrypt_init(enc) <= 0 ||
		    EVP_PKEY_CTX_set_rsa_padding(enc, RSA_PKCS1_OAEP_PADDING) <= 0 ||
		    EVP_PKEY_CTX_set_rsa_oaep_md(enc, EVP_sha1()) <= 0 || EVP_PKEY_CTX_set_rsa_mgf1_md(enc, EVP_sha1()) <= 0)
			break;
		size_t out_len = 0;
		if (EVP_PKEY_encrypt(enc, nullptr, &out_len, data, size) <= 0)
			break;
		bytes out(out_len);
		if (EVP_PKEY_encrypt(enc, out.data(), &out_len, data, size) <= 0)
			break;
		out.resize(out_len);
		result = std::move(out);
	} while (false);
	EVP_PKEY_CTX_free(enc);
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(from);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(bn_e);
	BN_free(bn_n);
	return result;
}
} // namespace od
