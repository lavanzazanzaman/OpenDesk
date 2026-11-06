#include "wineopenxr_fix.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

namespace od
{
namespace
{
constexpr char marker[] = "VDLNX-WOXR1"; // follows the injected code: identifies a patched DLL

using bytes = std::vector<uint8_t>;

template <typename T>
T get(const bytes & d, size_t o)
{
	T v{};
	if (o + sizeof(T) <= d.size())
		std::memcpy(&v, d.data() + o, sizeof(T));
	return v;
}

struct section
{
	uint32_t vsize, va, rawsize, rawptr;
	size_t header;
};

struct pe_file
{
	bytes d;
	std::vector<section> sections;
	std::map<std::string, uint32_t> symbols; // name -> RVA

	bool parse(std::string & why)
	{
		if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z')
			return why = "not a PE file", false;
		uint32_t pe = get<uint32_t>(d, 0x3c);
		if (pe + 24 > d.size() || std::memcmp(d.data() + pe, "PE\0\0", 4))
			return why = "not a PE file", false;
		if (get<uint16_t>(d, pe + 4) != 0x8664)
			return why = "not x86-64", false;
		uint16_t nsec = get<uint16_t>(d, pe + 6);
		uint32_t symtab = get<uint32_t>(d, pe + 12), nsym = get<uint32_t>(d, pe + 16);
		uint16_t optsize = get<uint16_t>(d, pe + 20);
		for (uint16_t i = 0; i < nsec; ++i)
		{
			size_t o = pe + 24 + optsize + 40 * size_t(i);
			sections.push_back({get<uint32_t>(d, o + 8), get<uint32_t>(d, o + 12), get<uint32_t>(d, o + 16), get<uint32_t>(d, o + 20), o});
		}
		if (!symtab || !nsym || symtab + 18ull * nsym > d.size())
			return why = "no symbol table", false;
		size_t strtab = symtab + 18ull * nsym;
		for (uint32_t i = 0; i < nsym; ++i)
		{
			size_t o = symtab + 18ull * i;
			std::string name;
			if (get<uint32_t>(d, o) == 0)
			{
				size_t s = strtab + get<uint32_t>(d, o + 4);
				while (s < d.size() && d[s])
					name.push_back(char(d[s++]));
			}
			else
				for (size_t k = 0; k < 8 && d[o + k]; ++k)
					name.push_back(char(d[o + k]));
			uint32_t value = get<uint32_t>(d, o + 8);
			int16_t secnum = get<int16_t>(d, o + 12);
			uint8_t naux = d[o + 17];
			if (secnum > 0 && size_t(secnum) <= sections.size())
				symbols.emplace(name, sections[size_t(secnum) - 1].va + value);
			i += naux;
		}
		return true;
	}

	const section * section_of(uint32_t rva) const
	{
		for (const auto & s: sections)
			if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawsize))
				return &s;
		return nullptr;
	}

	std::optional<size_t> off(uint32_t rva) const
	{
		const section * s = section_of(rva);
		if (!s)
			return std::nullopt;
		return size_t(s->rawptr) + (rva - s->va);
	}
};

struct plan
{
	uint32_t site = 0, cave = 0;
	bytes code; // injected code + marker
	const section * text = nullptr;
};

bool contains(const bytes & d, size_t from, size_t len, const bytes & needle)
{
	if (from >= d.size())
		return false;
	auto begin = d.begin() + ptrdiff_t(from), end = d.begin() + ptrdiff_t(std::min(d.size(), from + len));
	return std::search(begin, end, needle.begin(), needle.end()) != end;
}

void rel32(bytes & c, uint32_t next_rva, uint32_t target)
{
	int32_t r = int32_t(target - next_rva);
	uint8_t b[4];
	std::memcpy(b, &r, 4);
	c.insert(c.end(), b, b + 4);
}

// checks the build and works out the patch; state patched / needs_patch / unsupported
wineopenxr_state analyse(pe_file & f, plan & p, std::string & detail)
{
	if (std::search(f.d.begin(), f.d.end(), marker, marker + sizeof(marker) - 1) != f.d.end())
		return detail = "patched", wineopenxr_state::patched;
	if (!f.parse(detail))
		return wineopenxr_state::unsupported;
	auto & s = f.symbols;
	for (const char * n: {"xrDestroySession", "xrCreateSession", "session_list_lock", "__imp_free", "__imp_EnterCriticalSection",
	                      "__imp_LeaveCriticalSection"})
		if (!s.count(n))
			return detail = std::string("unrecognised build (no symbol ") + n + ")", wineopenxr_state::unsupported;
	uint32_t destroy = s["xrDestroySession"], create = s["xrCreateSession"], lock = s["session_list_lock"];
	uint32_t imp_free = s["__imp_free"], enter = s["__imp_EnterCriticalSection"], leave = s["__imp_LeaveCriticalSection"];

	// fixed builds take session_list_lock in xrDestroySession (to list_remove) - ours doesn't touch the list there
	if (auto o = f.off(destroy))
		for (size_t i = 0; i + 6 <= 0x100 && *o + i + 6 <= f.d.size(); ++i)
			if (f.d[*o + i] == 0xff && f.d[*o + i + 1] == 0x15 &&
			    destroy + uint32_t(i) + 6 + uint32_t(get<int32_t>(f.d, *o + i + 2)) == enter)
				return detail = "fixed in this build", wineopenxr_state::fixed;

	// session wrapper layout (list entry at +0x18/+0x20) from xrCreateSession's list_add_tail
	auto create_off = f.off(create);
	if (!create_off || !contains(f.d, *create_off, 0x300, {0x48, 0x8d, 0x43, 0x18, 0x48, 0x89, 0x7b, 0x18}) ||
	    !contains(f.d, *create_off, 0x300, {0x48, 0x89, 0x53, 0x20}))
		return detail = "unrecognised build (session list layout)", wineopenxr_state::unsupported;

	// xrDestroySession success path: mov rcx,rbx; call [__imp_free]; mov eax,esi; add rsp,0x40; pop rbx/rsi/rbp; ret
	auto destroy_off = f.off(destroy);
	static const uint8_t epilogue[] = {0x89, 0xf0, 0x48, 0x83, 0xc4, 0x40, 0x5b, 0x5e, 0x5d, 0xc3};
	for (size_t i = 0; destroy_off && i + 19 < 0x100 && *destroy_off + i + 19 < f.d.size(); ++i)
	{
		const uint8_t * c = f.d.data() + *destroy_off + i;
		if (c[0] == 0x48 && c[1] == 0x89 && c[2] == 0xd9 && c[3] == 0xff && c[4] == 0x15 &&
		    destroy + uint32_t(i) + 9 + uint32_t(get<int32_t>(f.d, *destroy_off + i + 5)) == imp_free &&
		    !std::memcmp(c + 9, epilogue, sizeof(epilogue)))
		{
			p.site = destroy + uint32_t(i);
			break;
		}
	}
	if (!p.site)
		return detail = "unrecognised build (xrDestroySession)", wineopenxr_state::unsupported;

	p.text = f.section_of(destroy);
	p.cave = (p.text->va + p.text->vsize + 15) & ~15u;
	bytes & c = p.code;
	auto lea_rcx = [&](uint32_t to) {
		c.insert(c.end(), {0x48, 0x8d, 0x0d});
		rel32(c, p.cave + uint32_t(c.size()) + 4, to);
	};
	auto call = [&](uint32_t to) {
		c.insert(c.end(), {0xff, 0x15});
		rel32(c, p.cave + uint32_t(c.size()) + 4, to);
	};
	lea_rcx(lock);
	call(enter);
	c.insert(c.end(), {0x48, 0x8b, 0x43, 0x18}); // mov rax,[rbx+0x18]  entry.next
	c.insert(c.end(), {0x48, 0x8b, 0x53, 0x20}); // mov rdx,[rbx+0x20]  entry.prev
	c.insert(c.end(), {0x48, 0x89, 0x02});       // mov [rdx],rax       prev->next = next
	c.insert(c.end(), {0x48, 0x89, 0x50, 0x08}); // mov [rax+8],rdx     next->prev = prev
	lea_rcx(lock);
	call(leave);
	c.insert(c.end(), {0x48, 0x89, 0xd9}); // mov rcx,rbx
	call(imp_free);
	c.insert(c.end(), epilogue, epilogue + sizeof(epilogue));
	c.insert(c.end(), marker, marker + sizeof(marker) - 1);
	auto cave_off = f.off(p.cave);
	if (p.cave + c.size() > p.text->va + p.text->rawsize || !cave_off || *cave_off + c.size() > f.d.size() ||
	    std::any_of(f.d.begin() + ptrdiff_t(*cave_off), f.d.begin() + ptrdiff_t(*cave_off + c.size()), [](uint8_t b) { return b; }))
		return detail = "unrecognised build (no room for the fix)", wineopenxr_state::unsupported;
	return detail = "has the session-list bug (Oculus games crash ~30 s in)", wineopenxr_state::needs_patch;
}

bool fail(std::string * error, const std::string & why)
{
	if (error)
		*error = why;
	return false;
}

bool read_file(const std::string & path, bytes & d)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	d.assign(std::istreambuf_iterator<char>(in), {});
	return true;
}
} // namespace

wineopenxr_state check_wineopenxr(const std::string & dll, std::string * detail)
{
	pe_file f;
	std::string why;
	if (!read_file(dll, f.d))
	{
		if (detail)
			*detail = "cannot read " + dll;
		return wineopenxr_state::error;
	}
	plan p;
	auto state = analyse(f, p, why);
	if (detail)
		*detail = why;
	return state;
}

bool patch_wineopenxr(const std::string & dll, std::string * error)
{
	pe_file f;
	if (!read_file(dll, f.d))
		return fail(error, "cannot read " + dll);
	plan p;
	std::string why;
	auto state = analyse(f, p, why);
	if (state == wineopenxr_state::patched)
		return true;
	if (state != wineopenxr_state::needs_patch)
		return fail(error, why);
	std::copy(p.code.begin(), p.code.end(), f.d.begin() + ptrdiff_t(*f.off(p.cave)));
	size_t site = *f.off(p.site);
	bytes jmp{0xe9};
	rel32(jmp, p.site + 5, p.cave);
	jmp.insert(jmp.end(), 4, 0x90);
	std::copy(jmp.begin(), jmp.end(), f.d.begin() + ptrdiff_t(site));
	uint32_t vsize = p.cave + uint32_t(p.code.size()) - p.text->va; // .text must cover the new code
	std::memcpy(f.d.data() + p.text->header + 8, &vsize, 4);
	// write next to it and rename: never a half-written DLL
	std::string tmp = dll + ".vdfix.tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out.write(reinterpret_cast<const char *>(f.d.data()), std::streamsize(f.d.size())))
			return fail(error, "cannot write " + tmp);
	}
	if (std::rename(tmp.c_str(), dll.c_str()) != 0)
	{
		std::remove(tmp.c_str());
		return fail(error, "cannot replace " + dll);
	}
	return true;
}
} // namespace od
