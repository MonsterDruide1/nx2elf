#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include "elf.h"
#include "elf_eh.h"
#include "lz4.h"
#include "types.h"

namespace fs = std::filesystem;

namespace File {

struct FileDeleter {
  typedef std::FILE* pointer;
  void operator()(FILE* f) { std::fclose(f); }
};
typedef std::unique_ptr<std::FILE, FileDeleter> UniqueFile;

static void iter_files(const fs::path& directory,
                       std::function<void(const fs::path&)> func) {
  for (auto& dirent : fs::directory_iterator(directory)) {
    auto& path = dirent.path();
    if (!fs::is_directory(path)) {
      func(path);
    }
  }
}

static UniqueFile Open(const fs::path& path, const char* mode) {
  return UniqueFile{fopen(path.string().c_str(), mode)};
}

static std::vector<u8> Read(const fs::path& path) {
  std::error_code error;
  auto size = fs::file_size(path, error);
  if (size == std::numeric_limits<std::uintmax_t>::max())
    return {};
  auto buffer = std::vector<u8>(size);
  auto f = Open(path, "rb");
  if (!f)
    return {};
  if (!fread(buffer.data(), buffer.size(), 1, f.get()))
    return {};
  return buffer;
}

static bool Write(const fs::path& path, const std::vector<u8>& buffer) {
  auto f = Open(path, "wb");
  if (!f)
    return false;
  return !!fwrite(buffer.data(), buffer.size(), 1, f.get());
}

};  // namespace File

struct StringTable {
  StringTable() { AddString(""); }
  void AddString(const char* str) {
    if (!entries.count(str)) {
      entries[str] = watermark;
      watermark += static_cast<u32>(strlen(str)) + sizeof(*str);
    }
  }
  u32 GetOffset(const char* str) {
    if (!entries.count(str)) {
      return 0;
    }
    return entries[str];
  }
  std::vector<char> GetBuffer() {
    std::vector<char> buffer(watermark);
    for (const auto& entry : entries) {
      strcpy(&buffer[entry.second], entry.first);
    }
    return buffer;
  }
  void Finalize() {
    buffer = GetBuffer();
    size = ALIGN_UP(buffer.size(), 0x10);
  }
  std::unordered_map<const char*, u32> entries;
  u32 watermark{};
  u64 offset;
  u64 size;
  std::vector<char> buffer;
};

struct NsoFile {
  enum FileType {
    kUnknown,
    kNso,
    kNro,
    kMod,
  };
  enum SegmentType { kText, kRodata, kData, kNumSegment };
  static const std::array<u8, 4> nso_magic;
  static const std::array<u8, 4> nro_magic;
  static const std::array<u8, 4> mod_magic;
  struct SegmentHeader {
    u32 file_offset;  // maybe &1==compressed?
    u32 mem_offset;
    u32 mem_size;
    u32 bss_align;
  };
  struct DataExtent {
    u32 offset;
    u32 size;
  };
  struct NsoHeader {
    u8 magic[4];
    u32 field_4;
    u32 field_8;
    u32 flags;
    SegmentHeader segments[kNumSegment];
    // value from .note, can be various lengths :/
    std::array<u8, 32> gnu_build_id;
    u32 segment_file_sizes[kNumSegment];
    u32 field_6c[9];
    DataExtent dynstr;
    DataExtent dynsym;
    sha256_digest segment_digests[kNumSegment];
  };
  // NRO stores the flat memory image - nothing needs to be decompressed or
  // relocated (although relocation fixups need to be applied). This also
  // implies that +4 in the file points to MOD header, so NRO header is at
  // offset 0x10 instead of 0.
  struct NroHeader {
    u8 magic[4];
    u32 field_4;
    u32 file_size;
    u32 field_c;
    DataExtent segments[kNumSegment];
    u32 bss_size;
    u32 field_3c;
    std::array<u8, 32> gnu_build_id;
    u32 field_60[4];
    DataExtent dynstr;
    DataExtent dynsym;
  };
  struct ModPointer {
    u32 field_0;
    u32 magic_offset;
  };
  struct ModHeader {
    // yaya, there are some fields here...for parsing, easier to ignore.
    // ModPointer mod_ptr;
    u8 magic[4];
    s32 dynamic_offset;
    s32 bss_start_offset;
    s32 bss_end_offset;
    s32 eh_start_offset;
    s32 eh_end_offset;
    s32 module_object_offset;
    // It seems the area around MOD0 is used for .note section
    // There is also a nss-name section
  };
  template <typename T>
  char* FormatBytes(char* p, T d) {
    for (auto& b : d)
      p += sprintf(p, "%02x", b);
    return p;
  }
  void Dump(bool verbose = false) {
    char msg[1024];
    char* p = msg;
    const char* idx2prot[kNumSegment] = {"r-x", "r--", "rw-"};

#define FMT_FIELD(f) p += sprintf(p, #f ": %8x\n", header.f);

		if (verbose) {
			FMT_FIELD(field_4);
			FMT_FIELD(field_8);
			FMT_FIELD(flags);
		}

    p += sprintf(p, "gnu_build_id: ");
    p = FormatBytes(p, header.gnu_build_id);
    p += sprintf(p, "\n");

    p += sprintf(p, "         %-8s %-8s %-8s %-8s %-8s\n", "file off",
                 "file len", "mem off", "mem len", "bss/algn");
    for (int i = 0; i < kNumSegment; i++) {
      auto& seg = header.segments[i];
      auto& file_size = header.segment_file_sizes[i];
      p += sprintf(p, "%d [%-3s]: %8x %8x %8x %8x %8x\n", i, idx2prot[i],
                   seg.file_offset, file_size, seg.mem_offset, seg.mem_size,
                   seg.bss_align);
    }

    if (verbose) {
      for (int i = 0; i < ARRAY_SIZE(header.field_6c); i++)
        FMT_FIELD(field_6c[i]);
    }

    p += sprintf(p, ".rodata-relative:\n");
    p += sprintf(p, "  .dynstr: %8x %8x\n", header.dynstr.offset,
                 header.dynstr.size);
    p += sprintf(p, "  .dynsym: %8x %8x\n", header.dynsym.offset,
                 header.dynsym.size);

    p += sprintf(p, "segment digests:\n");
    for (int i = 0; i < kNumSegment; i++) {
      p += sprintf(p, "%d [%-3s]: ", i, idx2prot[i]);
      p = FormatBytes(p, header.segment_digests[i]);
      p += sprintf(p, "\n");
    }

#undef FMT_FIELD

    printf("%s", msg);
  }
  bool Decompress(u8* dst, u32 dst_len, const u8* src, u32 src_len) {
    int len =
        LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                            reinterpret_cast<char*>(dst), src_len, dst_len);
    if (len != dst_len)
      printf("LZ4_decompress_safe: %d (expected %8x)\n", len, dst_len);
    return len > 0;
  }
  bool ResolvePlt(void* base, size_t len) {
    // Each plt slot is 4 instructions. The first entry fills 2 slots (resolving
    // thunk).
    if (dyn_info.pltrelsz) {
      const u32 plt_pattern[]{0xa9bf7bf0, 0xd00004d0, 0xf9428a11, 0x91144210,
                              0xd61f0220, 0xd503201f, 0xd503201f, 0xd503201f};
      const u32 plt_mask[ARRAY_SIZE(plt_pattern)]{
          0xffffffff, 0x00000000, 0xff000000, 0xff000000,
          0xff000000, 0xffffffff, 0xffffffff, 0xffffffff};
      auto found = static_cast<u8*>(
          memmem_m(base, len, plt_pattern, plt_mask, sizeof(plt_pattern)));
      if (found) {
        plt_info.addr = found - &image[0];
        // Assume the plt exactly matches .rela.plt
        u64 plt_entry_count = dyn_info.pltrelsz / sizeof(Elf64_Rela);
        const u64 plt_entry_size = sizeof(u32) * 4;
        plt_info.size = plt_entry_size * 2 + plt_entry_size * plt_entry_count;
        return true;
      }
    }
    return false;
  }
  bool Load(const fs::path& path) {
    auto file = File::Read(path);
    const size_t nro_offset = ALIGN_UP(sizeof(ModPointer), 0x10);
    if (file.size() >= sizeof(NsoHeader) &&
        !memcmp(&file[0], &nso_magic[0], nso_magic.size())) {
      memcpy(&header, &file[0], sizeof(header));

      // assume segments are after each other and mem offsets are aligned
      // note: there are also symbols "_start" and "end" which describe
      // the total size.
      auto& data_seg = header.segments[kData];
      size_t image_size =
          data_seg.mem_offset + data_seg.mem_size + data_seg.bss_align;
      image = std::vector<u8>(image_size);

      for (int i = 0; i < kNumSegment; i++) {
        auto& seg = header.segments[i];
        auto& file_size = header.segment_file_sizes[i];
				if ((header.flags & (1 << i)) != 0) {
					if (!Decompress(&image[seg.mem_offset], seg.mem_size,
						&file[seg.file_offset], file_size)) {
						return false;
					}
				} else {
					std::memcpy(&image[seg.mem_offset], &file[seg.file_offset], file_size);
				}
      }
      file_type = kNso;
    } else if (file.size() >= nro_offset + sizeof(NroHeader) &&
               !memcmp(&file[nro_offset], &nro_magic[0], nro_magic.size())) {
      // Translate the nro header to nso, which is a superset
      auto nro = reinterpret_cast<NroHeader*>(&file[nro_offset]);
      if (nro->file_size != file.size()) {
        return false;
      }
      for (int i = 0; i < kNumSegment; i++) {
        auto& seg = header.segments[i];
        // TODO revisit once some nso with uncompressed segments is seen
        seg.mem_offset = seg.file_offset = nro->segments[i].offset;
        seg.mem_size = header.segment_file_sizes[i] = nro->segments[i].size;
        switch (i) {
        case kText:
          seg.bss_align = 0x100;
          break;
        case kRodata:
          seg.bss_align = 1;
          break;
        case kData:
          seg.bss_align = nro->bss_size;
          break;
        }
      }
      header.gnu_build_id = nro->gnu_build_id;
      header.dynstr = nro->dynstr;
      header.dynsym = nro->dynsym;

      image = std::move(file);
      file_type = kNro;
    }

    u8* mod_base = nullptr;
    ModPointer* mod_ptr = nullptr;
    if (file_type != kUnknown) {
      mod_ptr = reinterpret_cast<ModPointer*>(&image[0]);
      if (mod_ptr->magic_offset + sizeof(ModHeader) > image.size()) {
        return false;
      }
      mod_base = &image[mod_ptr->magic_offset];
    } else if (file.size() >= sizeof(ModPointer)) {
      // It's not an NSO or NRO, but still need to check for MOD
      mod_ptr = reinterpret_cast<ModPointer*>(&file[0]);
      if (mod_ptr->magic_offset + sizeof(ModHeader) > file.size()) {
        return false;
      }
      mod_base = &file[mod_ptr->magic_offset];
    } else {
      return false;
    }
    auto mod = reinterpret_cast<ModHeader*>(mod_base);
    if (memcmp(mod->magic, &mod_magic[0], mod_magic.size()))
      return false;

    if (file_type == kUnknown) {
      // Apparently there are images which are essentially NROs, but lack
      // the NRO header, for some reason. This is a pain.
      image = std::move(file);
      file_type = kMod;
    }
    /*
    fs::path dump(path);
    File::Write(dump.replace_extension(".bin"), image);
    //*/

    auto mod_get_offset = [&](s32 relative_offset) {
      auto ptr = reinterpret_cast<u8*>(mod_base + relative_offset);
      auto offset = reinterpret_cast<uintptr_t>(ptr) -
                    reinterpret_cast<uintptr_t>(&image[0]);
      return static_cast<u32>(offset);
    };

    dynamic = reinterpret_cast<Elf64_Dyn*>(mod_base + mod->dynamic_offset);
    for (auto dyn = dynamic; dyn->d_tag; dyn++) {
#define DT_ASSIGN_U64(dt, var) \
  case dt:                     \
    dyn_info.var = dyn->d_un;  \
    break;
      switch (dyn->d_tag) {
        DT_ASSIGN_U64(DT_SYMTAB, symtab);
        DT_ASSIGN_U64(DT_RELA, rela);
        DT_ASSIGN_U64(DT_RELASZ, relasz);
        DT_ASSIGN_U64(DT_JMPREL, jmprel);
        DT_ASSIGN_U64(DT_PLTRELSZ, pltrelsz);
        DT_ASSIGN_U64(DT_STRTAB, strtab);
        DT_ASSIGN_U64(DT_STRSZ, strsz);
        DT_ASSIGN_U64(DT_PLTGOT, pltgot);
        DT_ASSIGN_U64(DT_HASH, hash);
        DT_ASSIGN_U64(DT_GNU_HASH, gnu_hash);
        DT_ASSIGN_U64(DT_INIT, init);
        DT_ASSIGN_U64(DT_FINI, fini);
        DT_ASSIGN_U64(DT_INIT_ARRAY, init_array);
        DT_ASSIGN_U64(DT_INIT_ARRAYSZ, init_arraysz);
        DT_ASSIGN_U64(DT_FINI_ARRAY, fini_array);
        DT_ASSIGN_U64(DT_FINI_ARRAYSZ, fini_arraysz);
      }
#undef DT_ASSIGN_U64
    }
    if (file_type != kMod) {
      auto& text_seg = header.segments[kText];
      ResolvePlt(&image[text_seg.mem_offset], text_seg.mem_size);
    }

    if (file_type == kMod) {
      // need to manually build them ...
      DataExtent segments[kNumSegment]{};

      // XXX hacks...
      if (!ResolvePlt(&image[0], image.size())) {
        fputs("error: raw MOD requires .plt. please report this.\n", stderr);
        return false;
      }
      if (dyn_info.symtab >= dyn_info.strtab) {
        fputs(
            "error: raw MOD requires .dynstr directly after .dynsym. please "
            "report this.\n",
            stderr);
        return false;
      }
      // Need this up-front to be able to iter_dynsym
      header.dynsym.size = static_cast<u32>(dyn_info.strtab - dyn_info.symtab);
      // yet another dirty hack. relies on all sections having at least
      // one symbol pointing into them, and a section symbol existing for .data
      std::vector<u16> seen_shndx;
      iter_dynsym([&](const Elf64_Sym& sym, u32) {
        if (sym.st_shndx == SHN_UNDEF || sym.st_shndx >= SHN_LORESERVE) {
          return;
        }
        seen_shndx.push_back(sym.st_shndx);
      });
      std::sort(seen_shndx.begin(), seen_shndx.end());
      seen_shndx.erase(std::unique(seen_shndx.begin(), seen_shndx.end()),
                       seen_shndx.end());
      if (seen_shndx.size() != kNumSegment + 1) {
        fputs(
            "error: raw MOD failed to find .data in .dynsym. please report "
            "this.\n",
            stderr);
        return false;
      }
      iter_dynsym([&](const Elf64_Sym& sym, u32) {
        if (segments[kData].offset == 0 &&
            ELF64_ST_TYPE(sym.st_info) == STT_SECTION &&
            sym.st_shndx == seen_shndx[kData]) {
          segments[kData].offset = static_cast<u32>(sym.st_value);
        }
      });
      if (segments[kData].offset == 0) {
        fputs(
            "error: raw MOD failed to find .data in .dynsym. please report "
            "this.\n",
            stderr);
        return false;
      }

      segments[kText].offset = 0;
      segments[kText].size = static_cast<u32>(plt_info.addr + plt_info.size);
      segments[kRodata].offset =
          ALIGN_UP(segments[kText].offset + segments[kText].size, 0x1000);
      segments[kRodata].size =
          segments[kData].offset - segments[kRodata].offset;
      segments[kData].size =
          static_cast<u32>(image.size() - segments[kData].offset);

      header.dynstr.offset =
          static_cast<u32>(dyn_info.strtab - segments[kRodata].offset);
      header.dynstr.size = static_cast<u32>(dyn_info.strsz);
      header.dynsym.offset =
          static_cast<u32>(dyn_info.symtab - segments[kRodata].offset);

      for (int i = 0; i < kNumSegment; i++) {
        auto& seg = header.segments[i];
        seg.mem_offset = seg.file_offset = segments[i].offset;
        seg.mem_size = header.segment_file_sizes[i] = segments[i].size;
        switch (i) {
        case kText:
          seg.bss_align = 0x100;
          break;
        case kRodata:
          seg.bss_align = 1;
          break;
        case kData:
          // This is the actual size cleared by init code, but there
          // is a symbol named "end" which will be referenced and is
          // at the aligned boundary. So pad it out until there.
          // This is debatably a bug in nintendo's tools.
          seg.bss_align = mod_get_offset(mod->bss_end_offset) -
                          mod_get_offset(mod->bss_start_offset);
          seg.bss_align = ALIGN_UP(seg.bss_align, 0x1000) + 1;
          break;
        }
      }
    }

    // Kinda gross, but hopefully unique enough to avoid false positives...
    const GnuBuildId md5_build_id_needle = {
        {sizeof(GnuBuildId::owner), sizeof(GnuBuildId::build_id_md5), 3},
        {'G', 'N', 'U'}};
    const GnuBuildId sha1_build_id_needle = {
        {sizeof(GnuBuildId::owner), sizeof(GnuBuildId::build_id_sha1), 3},
        {'G', 'N', 'U'}};
    for (auto i : {kRodata, kText, kData}) {
      auto& seg = header.segments[i];
      note = reinterpret_cast<Elf64_Nhdr*>(
          memmemr(&image[seg.mem_offset], seg.mem_size, &md5_build_id_needle,
                  offsetof(GnuBuildId, build_id_md5)));
      if (note) {
        break;
      }
      note = reinterpret_cast<Elf64_Nhdr*>(
          memmemr(&image[seg.mem_offset], seg.mem_size, &sha1_build_id_needle,
                  offsetof(GnuBuildId, build_id_sha1)));
      if (note) {
        break;
      }
    }

    // In case of MOD-only file, we can only fill in build id if the section was
    // found manually
    if (file_type == kMod && note) {
      auto build_id = reinterpret_cast<const GnuBuildId*>(note);
      memcpy(header.gnu_build_id.data(), build_id->build_id_raw.data(),
             build_id->header.n_descsz);
    }

    eh_info.hdr_addr = mod_get_offset(mod->eh_start_offset);
    eh_info.hdr_size = mod_get_offset(mod->eh_end_offset) - eh_info.hdr_addr;

    return true;
  }
  void DumpElfInfo() {
    puts("dynamic:");
    struct {
      Elf64_Rela* rela;
      u64 num_rela;
      Elf64_Rela* jmprel;
      u64 num_jmprel;
    } rela_info;
    for (auto dyn = dynamic; dyn->d_tag; dyn++) {
      printf("%16" PRIx64 " %16" PRIx64 "\n", dyn->d_tag, dyn->d_un);

#define DT_ASSIGN_PTR(dt, var, x)                                   \
  case dt:                                                          \
    rela_info.var = reinterpret_cast<decltype(rela_info.var)>((x)); \
    break;
#define DT_ASSIGN_U64(dt, var, x)                              \
  case dt:                                                     \
    rela_info.var = static_cast<decltype(rela_info.var)>((x)); \
    break;

      switch (dyn->d_tag) {
        DT_ASSIGN_PTR(DT_RELA, rela, &image[dyn->d_un]);
        DT_ASSIGN_U64(DT_RELASZ, num_rela, dyn->d_un / sizeof(*rela_info.rela));
        DT_ASSIGN_PTR(DT_JMPREL, jmprel, &image[dyn->d_un]);
        DT_ASSIGN_U64(DT_PLTRELSZ, num_jmprel,
                      dyn->d_un / sizeof(*rela_info.jmprel));
      }
#undef DT_ASSIGN_U64
#undef DT_ASSIGN_PTR
    }
    puts("rela:");
    for (size_t i = 0; i < rela_info.num_rela; i++) {
      auto& rela = rela_info.rela[i];
      printf("%16" PRIx64 " %8x %8x %16" PRIx64 "\n", rela.r_offset,
             ELF64_R_SYM(rela.r_info), ELF64_R_TYPE(rela.r_info),
             rela.r_addend);
    }
    puts("jmprel:");
    for (size_t i = 0; i < rela_info.num_jmprel; i++) {
      auto& rela = rela_info.jmprel[i];
      printf("%16" PRIx64 " %8x %8x %16" PRIx64 "x\n", rela.r_offset,
             ELF64_R_SYM(rela.r_info), ELF64_R_TYPE(rela.r_info),
             rela.r_addend);
    }

    auto rodata = &image[header.segments[kRodata].mem_offset];
    auto dynstr = reinterpret_cast<const char*>(&rodata[header.dynstr.offset]);
    puts("symbols:");
    iter_dynsym([&](const Elf64_Sym& sym, u32) {
      auto name = &dynstr[sym.st_name];
      printf("%x %x %x %4x %16" PRIx64 " %16" PRIx64 " %s\n",
             ELF64_ST_BIND(sym.st_info), ELF64_ST_TYPE(sym.st_info),
             ELF64_ST_VISIBILITY(sym.st_other), sym.st_shndx, sym.st_value,
             sym.st_size, name);
    });
  }
  void iter_dynsym(std::function<void(const Elf64_Sym&, u32)> func) {
    auto sym = reinterpret_cast<Elf64_Sym*>(&image[dyn_info.symtab]);
    for (u32 i = 0; i < header.dynsym.size / sizeof(Elf64_Sym); i++, sym++) {
      func(*sym, i);
    }
  }
  bool WriteUncompressedNso(const fs::path& path) {
    NsoHeader new_header = header;
    // clear compression flags
    new_header.flags &= 0xf8;
    // fix segment offsets and size
    for (int i = 0; i < kNumSegment; i++) {
      new_header.segments[i].file_offset = new_header.segments[i].mem_offset + sizeof(NsoHeader);
      new_header.segment_file_sizes[i] = new_header.segments[i].mem_size;
    }
    new_header.segments[kText].bss_align = 0x100;
    new_header.segments[kRodata].bss_align = 0;

    u32 image_size = new_header.segments[kData].mem_offset + 
                     new_header.segments[kData].mem_size;
    std::vector<u8> data = std::vector<u8>(sizeof(NsoHeader) + image_size);
    memcpy(data.data(), &new_header, sizeof(NsoHeader));
    memcpy(data.data() + sizeof(NsoHeader), image.data(), image_size);
    File::Write(path, data);
    return true;
  }
  bool WriteElf(const fs::path& path) {
    StringTable shstrtab;
    shstrtab.AddString(".shstrtab");

    // Profile sections based on dynsym
    u16 num_shdrs = 0;
    std::unordered_map<u16, Elf64_Shdr> known_sections;
    auto vaddr_to_shdr = [&](u64 vaddr) {
      Elf64_Shdr shdr{};
      for (int i = 0; i < kNumSegment; i++) {
        u64 location = vaddr;
        auto& seg = header.segments[i];
        auto seg_mem_end = seg.mem_offset + seg.mem_size;
        // sh_offset will be fixed up later
        if (location >= seg.mem_offset && location < seg_mem_end) {
          // .text, .data, .rodata
          const char* name = "";
          shdr.sh_type = SHT_PROGBITS;
          switch (i) {
          case kText:
            shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
            name = ".text";
            break;
          case kData:
            shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
            name = ".data";
            break;
          case kRodata:
            shdr.sh_flags = SHF_ALLOC;
            name = ".rodata";
            break;
          }
          shstrtab.AddString(name);
          shdr.sh_name = shstrtab.GetOffset(name);
          shdr.sh_addr = seg.mem_offset;
          shdr.sh_size = seg.mem_size;
          shdr.sh_addralign = sizeof(u64);
        } else if (i == kData && (location >= seg_mem_end &&
                                  location <= seg_mem_end + seg.bss_align)) {
          // .bss
          const char* name = ".bss";
          shstrtab.AddString(name);
          shdr.sh_name = shstrtab.GetOffset(name);
          shdr.sh_type = SHT_NOBITS;
          shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
          shdr.sh_addr = seg_mem_end;
          shdr.sh_size = seg.bss_align;
          shdr.sh_addralign = sizeof(u64);
        }
      }
      return shdr;
    };
    iter_dynsym([&](const Elf64_Sym& sym, u32) {
      if (sym.st_shndx >= SHN_LORESERVE) {
        return;
      }
      num_shdrs = std::max(num_shdrs, sym.st_shndx);
      if (sym.st_shndx != SHT_NULL && !known_sections.count(sym.st_shndx)) {
        auto shdr = vaddr_to_shdr(sym.st_value);
        if (shdr.sh_type != SHT_NULL) {
          known_sections[sym.st_shndx] = shdr;
        } else {
          fprintf(stderr, "failed to make shdr for st_shndx %d\n",
                  sym.st_shndx);
        }
      }
    });
    // Check if we need to manually add the known segments (nothing was pointing
    // to them, so they can go anywhere).
    if (known_sections.size() != kNumSegment + 1) {
      auto next_free = [&known_sections](u16 start) -> u16 {
        for (u16 i = start + 1; i < SHN_LORESERVE; i++) {
          if (!known_sections.count(i)) {
            return i;
          }
        }
        return SHN_UNDEF;
      };
      u16 shndx = next_free(SHN_UNDEF);
      if (shndx != SHN_UNDEF && !shstrtab.GetOffset(".text") &&
          header.segments[kText].mem_size > 0) {
        known_sections[shndx] =
            vaddr_to_shdr(header.segments[kText].mem_offset);
        shndx = next_free(shndx);
      }
      if (shndx != SHN_UNDEF && !shstrtab.GetOffset(".rodata") &&
          header.segments[kRodata].mem_size > 0) {
        known_sections[shndx] =
            vaddr_to_shdr(header.segments[kRodata].mem_offset);
        shndx = next_free(shndx);
      }
      if (shndx != SHN_UNDEF && !shstrtab.GetOffset(".data") &&
          header.segments[kData].mem_size > 0) {
        known_sections[shndx] =
            vaddr_to_shdr(header.segments[kData].mem_offset);
        shndx = next_free(shndx);
      }
      if (shndx != SHN_UNDEF && !shstrtab.GetOffset(".bss") &&
          header.segments[kData].bss_align > 0) {
        known_sections[shndx] =
            vaddr_to_shdr(header.segments[kData].mem_offset +
                          header.segments[kData].mem_size);
        shndx = next_free(shndx);
      }
    }
    // +1 to go from index -> count
    num_shdrs++;

    // Determine how many other sections are needed
    int shdrs_needed = static_cast<int>(known_sections.size()) - num_shdrs;
    // index 0
    shdrs_needed++;
    // .shstrtab
    shdrs_needed++;
    // Assume the following will always be present: .dynstr, .dynsym, .dynamic,
    // .rela.dyn
    for (auto& name : {".dynstr", ".dynsym", ".dynamic", ".rela.dyn"}) {
      shstrtab.AddString(name);
      shdrs_needed++;
    }

    struct {
      bool plt;
      bool got;
      bool got_plt;
      bool rela_plt;
      bool hash;
      bool gnu_hash;
      bool init;
      bool fini;
      bool init_array;
      bool fini_array;
      bool note;
      bool eh;
    } present{};
#define ALLOC_SHDR_IF(condition, name) \
  if ((condition)) {                   \
    present.name = true;               \
    shdrs_needed++;                    \
  }
    ALLOC_SHDR_IF(plt_info.addr, plt);
    u64 jump_slot_addr_end = 0;
    if (dyn_info.jmprel) {
      for (size_t i = 0; i < dyn_info.pltrelsz / sizeof(Elf64_Rela); i++) {
        auto& rela = reinterpret_cast<Elf64_Rela*>(&image[dyn_info.jmprel])[i];
        if (ELF64_R_TYPE(rela.r_info) == R_AARCH64_JUMP_SLOT) {
          jump_slot_addr_end =
              std::max(jump_slot_addr_end, rela.r_offset + sizeof(u64));
        }
      }
    }
    ALLOC_SHDR_IF(jump_slot_addr_end && dyn_info.pltgot, got_plt);
    u64 got_addr = 0;
    if (jump_slot_addr_end) {
      u64 got_dynamic_ptr = reinterpret_cast<uintptr_t>(dynamic) -
                            reinterpret_cast<uintptr_t>(&image[0]);
      auto found = static_cast<u8*>(
          memmem(&image[jump_slot_addr_end], image.size() - jump_slot_addr_end,
                 &got_dynamic_ptr, sizeof(got_dynamic_ptr)));
      if (found) {
        got_addr = found - &image[0];
      }
    }
    ALLOC_SHDR_IF(got_addr && dyn_info.rela, got);
    ALLOC_SHDR_IF(present.got_plt && dyn_info.jmprel && dyn_info.pltrelsz,
                  rela_plt);
    ALLOC_SHDR_IF(dyn_info.hash, hash);
    ALLOC_SHDR_IF(dyn_info.gnu_hash, gnu_hash);
    ALLOC_SHDR_IF(dyn_info.init_array && dyn_info.init_arraysz, init_array);
    ALLOC_SHDR_IF(dyn_info.fini_array && dyn_info.fini_arraysz, fini_array);
    ALLOC_SHDR_IF(note, note);
    u32 init_ret_offset = 0;
    if (dyn_info.init) {
      auto init_ptr = reinterpret_cast<u32*>(&image[dyn_info.init]);
      for (int i = 0;; i++) {
        if (init_ptr[i] == 0xd65f03c0ul) {
          init_ret_offset = (i + 1) * sizeof(u32);
          break;
        }
      }
      ALLOC_SHDR_IF(init_ret_offset, init);
    }
    u32 fini_branch_offset = 0;
    if (dyn_info.fini) {
      auto fini_ptr = reinterpret_cast<u32*>(&image[dyn_info.fini]);
      for (int i = 0; i < 0x20; i++) {
        if ((fini_ptr[i] & 0xff000000ul) == 0x14000000ul) {
          fini_branch_offset = (i + 1) * sizeof(u32);
          break;
        }
      }
      ALLOC_SHDR_IF(fini_branch_offset, fini);
    }
#undef ALLOC_SHDR_IF

    ElfEHInfo eh;
    uintptr_t eh_frame_ptr;
    if (eh.MeasureFrame(
            reinterpret_cast<eh_frame_hdr*>(&image[eh_info.hdr_addr]),
            &eh_frame_ptr, &eh_info.frame_size)) {
      eh_info.frame_addr =
          eh_info.hdr_addr + (eh_frame_ptr - reinterpret_cast<uintptr_t>(
                                                 &image[eh_info.hdr_addr]));
      // XXX the alignment of sizes is a fudge...
      eh_info.hdr_size = ALIGN_UP(eh_info.hdr_size, 0x10);
      eh_info.frame_size = ALIGN_UP(eh_info.frame_size, 0x10);
      present.eh = true;
      // Account for .eh_frame_hdr and .eh_frame
      shdrs_needed += 2;
      shstrtab.AddString(".eh_frame_hdr");
      shstrtab.AddString(".eh_frame");
    }

    if (present.plt)
      shstrtab.AddString(".plt");
    if (present.got)
      shstrtab.AddString(".got");
    if (present.got_plt)
      shstrtab.AddString(".got.plt");
    if (present.rela_plt)
      shstrtab.AddString(".rela.plt");
    if (present.hash)
      shstrtab.AddString(".hash");
    if (present.gnu_hash)
      shstrtab.AddString(".gnu.hash");
    if (present.init)
      shstrtab.AddString(".init");
    if (present.fini)
      shstrtab.AddString(".fini");
    if (present.init_array)
      shstrtab.AddString(".init_array");
    if (present.fini_array)
      shstrtab.AddString(".fini_array");
    if (present.note)
      shstrtab.AddString(".note");

    shstrtab.Finalize();
    if (shdrs_needed > 0) {
      num_shdrs += shdrs_needed;
    }

    // Add dynamic and EH segments
    u16 num_phdrs = kNumSegment + 2;

    size_t elf_size = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * num_phdrs +
                      sizeof(Elf64_Shdr) * num_shdrs;
    elf_size += shstrtab.size;
    for (auto& seg : header.segments) {
      elf_size += seg.mem_size;
    }
    std::vector<u8> elf(elf_size);

    auto ehdr = reinterpret_cast<Elf64_Ehdr*>(&elf[0]);
    ehdr->e_ident = {ELF_MAGIC,  ELFCLASS64,    ELFDATA2LSB,
                     EV_CURRENT, ELFOSABI_NONE, 0};
    ehdr->e_type = ET_DYN;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = EV_CURRENT;
    ehdr->e_ehsize = sizeof(Elf64_Ehdr);
    ehdr->e_flags = 0;
    ehdr->e_entry = header.segments[kText].mem_offset;
    ehdr->e_phoff = ehdr->e_ehsize;
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum = num_phdrs;
    ehdr->e_shoff = ehdr->e_phoff + ehdr->e_phentsize * ehdr->e_phnum;
    ehdr->e_shentsize = sizeof(Elf64_Shdr);
    ehdr->e_shnum = num_shdrs;
    ehdr->e_shstrndx = SHN_UNDEF;

    // IDA only _needs_ phdrs and dynamic phdr to give good results
    auto phdrs = reinterpret_cast<Elf64_Phdr*>(&elf[0] + ehdr->e_phoff);

    auto vaddr_to_foffset = [&](u64 vaddr) -> u64 {
      for (size_t i = 0; i < kNumSegment; i++) {
        auto phdr = &phdrs[i];
        if (vaddr >= phdr->p_vaddr && vaddr < phdr->p_vaddr + phdr->p_filesz) {
          return phdr->p_offset + (vaddr - phdr->p_vaddr);
        }
      }
      return 0;
    };

    shstrtab.offset = ehdr->e_shoff + ehdr->e_shentsize * ehdr->e_shnum;
    memcpy(&elf[shstrtab.offset], &shstrtab.buffer[0], shstrtab.buffer.size());

    size_t data_offset_cur = shstrtab.offset + shstrtab.size;
    for (size_t i = 0; i < num_phdrs; i++) {
      auto phdr = &phdrs[i];
      if (i < kNumSegment) {
        auto& seg = header.segments[i];
        phdr->p_type = PT_LOAD;
        switch (i) {
        case kText:
          phdr->p_flags = PF_R | PF_X;
          break;
        case kRodata:
          phdr->p_flags = PF_R;
          break;
        case kData:
          phdr->p_flags = PF_R | PF_W;
          break;
        }
        phdr->p_vaddr = phdr->p_paddr = seg.mem_offset;
        phdr->p_offset = data_offset_cur;
        phdr->p_filesz = seg.mem_size;
        if (i == kData) {
          phdr->p_memsz = seg.mem_size + seg.bss_align;
          phdr->p_align = 1;
        } else {
          phdr->p_memsz = seg.mem_size;
          phdr->p_align = std::max(1u, seg.bss_align);
        }

        memcpy(&elf[0] + phdr->p_offset, &image[seg.mem_offset],
               phdr->p_filesz);

        // fixup sh_offset
        for (auto& known_section : known_sections) {
          if (known_section.second.sh_addr == phdr->p_vaddr) {
            known_section.second.sh_offset = phdr->p_offset;
          }
        }

        data_offset_cur += phdr->p_filesz;
      } else if (i == kData + 1) {
        phdr->p_type = PT_DYNAMIC;
        phdr->p_flags = PF_R | PF_W;
        phdr->p_vaddr = phdr->p_paddr = reinterpret_cast<uintptr_t>(dynamic) -
                                        reinterpret_cast<uintptr_t>(&image[0]);
        phdr->p_offset = vaddr_to_foffset(phdr->p_vaddr);
        size_t dyn_size = sizeof(Elf64_Dyn);
        for (auto dyn = dynamic; dyn->d_tag; dyn++) {
          dyn_size += sizeof(Elf64_Dyn);
        }
        phdr->p_filesz = phdr->p_memsz = dyn_size;
        phdr->p_align = sizeof(u64);
      } else if (i == kData + 2) {
        // Too bad ida doesn't fucking use it!
        phdr->p_type = PT_GNU_EH_FRAME;
        phdr->p_flags = PF_R;
        phdr->p_vaddr = phdr->p_paddr = eh_info.hdr_addr;
        phdr->p_offset = vaddr_to_foffset(phdr->p_vaddr);
        phdr->p_filesz = phdr->p_memsz = eh_info.hdr_size;
        phdr->p_align = sizeof(u32);
      }
    }

    // IDA's elf loader will also look for certain sections...
    // IMO this is IDA bug - it should just use PT_DYNAMIC
    // At least on 6.95, IDA will do a decent job if only PT_DYNAMIC is
    // there, but once SHT_DYNAMIC is added, then many entries which would
    // otherwise work fine by being only in the dynamic section, must also
    // have section headers...
    auto shdrs = reinterpret_cast<Elf64_Shdr*>(&elf[0] + ehdr->e_shoff);
    // Insert sections for which section index was known
    for (auto& known_section : known_sections) {
      auto shdr = &shdrs[known_section.first];
      *shdr = known_section.second;
    }
    // Insert other handy sections at an available section index
    auto insert_shdr = [&](const Elf64_Shdr& shdr,
                           bool ordered = false) -> u32 {
      u32 start = 1;
      // This is basically a hack to convince ida not to delete segments
      if (ordered) {
        for (auto& known_section : known_sections) {
          auto& known_shdr = known_section.second;
          if (shdr.sh_addr >= known_shdr.sh_addr &&
              shdr.sh_addr < known_shdr.sh_addr + known_shdr.sh_size) {
            start = known_section.first + 1;
          }
        }
      }
    retry:
      for (u32 i = start; i < num_shdrs; i++) {
        if (shdrs[i].sh_type == SHT_NULL) {
          shdrs[i] = shdr;
          return i;
        }
      }
      // failed to find open spot with restrictions, so try again at any
      // location
      if (ordered && start != 1) {
        fprintf(stderr,
                "warning: failed to meet ordering for sh_addr %16" PRIx64 "\n",
                shdr.sh_addr);
        start = 1;
        goto retry;
      }
      return SHN_UNDEF;
    };

    Elf64_Shdr shdr;

    if (present.init) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".init");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
      shdr.sh_addr = dyn_info.init;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = init_ret_offset;
      shdr.sh_addralign = sizeof(u32);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .init", stderr);
      }
    }

    if (present.fini) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".fini");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
      shdr.sh_addr = dyn_info.fini;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = fini_branch_offset;
      shdr.sh_addralign = sizeof(u32);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .fini", stderr);
      }
    }

    shdr = {};
    shdr.sh_name = shstrtab.GetOffset(".dynstr");
    shdr.sh_type = SHT_STRTAB;
    shdr.sh_flags = SHF_ALLOC;
    shdr.sh_addr = header.segments[kRodata].mem_offset + header.dynstr.offset;
    shdr.sh_offset = phdrs[kRodata].p_offset + header.dynstr.offset;
    shdr.sh_size = header.dynstr.size;
    shdr.sh_addralign = sizeof(char);
    u32 dynstr_shndx = insert_shdr(shdr);
    if (dynstr_shndx == SHN_UNDEF) {
      fputs("failed to insert new shdr for .dynstr", stderr);
    }

    u32 last_local_dynsym_index = 0;
    iter_dynsym([&](const Elf64_Sym& sym, u32 index) {
      if (ELF64_ST_BIND(sym.st_info) == STB_LOCAL) {
        last_local_dynsym_index = std::max(last_local_dynsym_index, index);
      }
    });
    shdr = {};
    shdr.sh_name = shstrtab.GetOffset(".dynsym");
    shdr.sh_type = SHT_DYNSYM;
    shdr.sh_flags = SHF_ALLOC;
    shdr.sh_addr = header.segments[kRodata].mem_offset + header.dynsym.offset;
    shdr.sh_offset = phdrs[kRodata].p_offset + header.dynsym.offset;
    shdr.sh_size = header.dynsym.size;
    shdr.sh_link = dynstr_shndx;
    shdr.sh_info = last_local_dynsym_index + 1;
    shdr.sh_addralign = sizeof(u64);
    shdr.sh_entsize = sizeof(Elf64_Sym);
    u32 dynsym_shndx = insert_shdr(shdr);
    if (dynsym_shndx == SHN_UNDEF) {
      fputs("failed to insert new shdr for .dynsym", stderr);
    }

    auto dyn_phdr = &phdrs[kData + 1];
    shdr = {};
    shdr.sh_name = shstrtab.GetOffset(".dynamic");
    shdr.sh_type = SHT_DYNAMIC;
    shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
    shdr.sh_addr = dyn_phdr->p_vaddr;
    shdr.sh_offset = dyn_phdr->p_offset;
    shdr.sh_size = dyn_phdr->p_filesz;
    shdr.sh_link = dynstr_shndx;
    shdr.sh_addralign = dyn_phdr->p_align;
    shdr.sh_entsize = sizeof(Elf64_Dyn);
    if (insert_shdr(shdr) == SHN_UNDEF) {
      fputs("failed to insert new shdr for .dynamic", stderr);
    }

    shdr = {};
    shdr.sh_name = shstrtab.GetOffset(".rela.dyn");
    shdr.sh_type = SHT_RELA;
    shdr.sh_flags = SHF_ALLOC;
    shdr.sh_addr = dyn_info.rela;
    shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
    shdr.sh_size = dyn_info.relasz;
    shdr.sh_link = dynsym_shndx;
    shdr.sh_addralign = sizeof(u64);
    shdr.sh_entsize = sizeof(Elf64_Rela);
    if (insert_shdr(shdr) == SHN_UNDEF) {
      fputs("failed to insert new shdr for .rela.dyn", stderr);
    }

    u32 plt_shndx = SHN_UNDEF;
    if (present.plt) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".plt");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
      shdr.sh_addr = plt_info.addr;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = plt_info.size;
      shdr.sh_addralign = 0x10;
      shdr.sh_entsize = 0x10;
      plt_shndx = insert_shdr(shdr, true);
      if (plt_shndx == SHN_UNDEF) {
        fputs("failed to insert new shdr for .plt", stderr);
      }
    }

    if (present.got) {
      u64 glob_dat_end = got_addr;
      for (size_t i = 0; i < dyn_info.relasz / sizeof(Elf64_Rela); i++) {
        auto& rela = reinterpret_cast<Elf64_Rela*>(&image[dyn_info.rela])[i];
        if (ELF64_R_TYPE(rela.r_info) == R_AARCH64_GLOB_DAT) {
          glob_dat_end = std::max(glob_dat_end, rela.r_offset + sizeof(u64));
        }
      }
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".got");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
      shdr.sh_addr = got_addr;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = glob_dat_end - got_addr;
      shdr.sh_addralign = sizeof(u64);
      shdr.sh_entsize = sizeof(u64);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .got", stderr);
      }
    }

    if (present.got_plt) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".got.plt");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
      shdr.sh_addr = dyn_info.pltgot;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = jump_slot_addr_end - dyn_info.pltgot;
      shdr.sh_addralign = sizeof(u64);
      shdr.sh_entsize = sizeof(u64);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .got.plt", stderr);
      }
    }

    if (present.rela_plt) {
      if (!present.plt) {
        fputs("warning: .rela.plt with no .plt", stderr);
      }
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".rela.plt");
      shdr.sh_type = SHT_RELA;
      shdr.sh_flags = SHF_ALLOC;
      if (plt_shndx != SHN_UNDEF) {
        shdr.sh_flags |= SHF_INFO_LINK;
      }
      shdr.sh_addr = dyn_info.jmprel;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = dyn_info.pltrelsz;
      shdr.sh_link = dynsym_shndx;
      shdr.sh_info = plt_shndx;
      shdr.sh_addralign = sizeof(u64);
      shdr.sh_entsize = sizeof(Elf64_Rela);
      if (insert_shdr(shdr) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .rela.plt", stderr);
      }
    }

    if (present.init_array) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".init_array");
      shdr.sh_type = SHT_INIT_ARRAY;
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
      shdr.sh_addr = dyn_info.init_array;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = dyn_info.init_arraysz;
      shdr.sh_addralign = sizeof(u64);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .init_array", stderr);
      }
    }

    if (present.fini_array) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".fini_array");
      shdr.sh_type = SHT_FINI_ARRAY;
      shdr.sh_flags = SHF_ALLOC | SHF_WRITE;
      shdr.sh_addr = dyn_info.fini_array;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = dyn_info.fini_arraysz;
      shdr.sh_addralign = sizeof(u64);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .fini_array", stderr);
      }
    }

    if (present.hash) {
      struct {
        u32 nbucket;
        u32 nchain;
      }* hash = reinterpret_cast<decltype(hash)>(&image[dyn_info.hash]);
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".hash");
      shdr.sh_type = SHT_HASH;
      shdr.sh_flags = SHF_ALLOC;
      shdr.sh_addr = dyn_info.hash;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = sizeof(*hash) + hash->nbucket * sizeof(u32) +
                     hash->nchain * sizeof(u32);
      shdr.sh_link = dynsym_shndx;
      shdr.sh_addralign = sizeof(u64);
      shdr.sh_entsize = sizeof(u32);
      if (insert_shdr(shdr) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .hash", stderr);
      }
    }

    if (present.gnu_hash) {
      struct {
        u32 nbuckets;
        u32 symndx;
        u32 maskwords;
        u32 shift2;
      }* gnu_hash =
          reinterpret_cast<decltype(gnu_hash)>(&image[dyn_info.gnu_hash]);
      size_t gnu_hash_len = sizeof(*gnu_hash);
      gnu_hash_len += gnu_hash->maskwords * sizeof(u64);
      gnu_hash_len += gnu_hash->nbuckets * sizeof(u32);
      u64 dynsymcount = header.dynsym.size / sizeof(Elf64_Sym);
      gnu_hash_len += (dynsymcount - gnu_hash->symndx) * sizeof(u32);
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".gnu.hash");
      shdr.sh_type = SHT_GNU_HASH;
      shdr.sh_flags = SHF_ALLOC;
      shdr.sh_addr = dyn_info.gnu_hash;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = gnu_hash_len;
      shdr.sh_link = dynsym_shndx;
      shdr.sh_addralign = sizeof(u64);
      shdr.sh_entsize = sizeof(u32);
      if (insert_shdr(shdr) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .gnu.hash", stderr);
      }
    }

    if (present.note) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".note");
      shdr.sh_type = SHT_NOTE;
      shdr.sh_flags = SHF_ALLOC;
      shdr.sh_addr = reinterpret_cast<uintptr_t>(note) -
                     reinterpret_cast<uintptr_t>(&image[0]);
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = sizeof(*note) + note->n_descsz + note->n_namesz;
      shdr.sh_addralign = sizeof(u32);
      if (insert_shdr(shdr) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .note", stderr);
      }
    }

    if (present.eh) {
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".eh_frame_hdr");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC;
      shdr.sh_addr = eh_info.hdr_addr;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = eh_info.hdr_size;
      shdr.sh_addralign = sizeof(u32);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .eh_frame_hdr", stderr);
      }
      shdr = {};
      shdr.sh_name = shstrtab.GetOffset(".eh_frame");
      shdr.sh_type = SHT_PROGBITS;
      shdr.sh_flags = SHF_ALLOC;
      shdr.sh_addr = eh_info.frame_addr;
      shdr.sh_offset = vaddr_to_foffset(shdr.sh_addr);
      shdr.sh_size = eh_info.frame_size;
      shdr.sh_addralign = sizeof(u32);
      if (insert_shdr(shdr, true) == SHN_UNDEF) {
        fputs("failed to insert new shdr for .eh_frame", stderr);
      }
    }

    shdr = {};
    shdr.sh_name = shstrtab.GetOffset(".shstrtab");
    shdr.sh_type = SHT_STRTAB;
    shdr.sh_offset = shstrtab.offset;
    shdr.sh_size = shstrtab.buffer.size();
    shdr.sh_addralign = sizeof(char);
    ehdr->e_shstrndx = insert_shdr(shdr);
    if (ehdr->e_shstrndx == SHN_UNDEF) {
      fputs("failed to insert new shdr for .shstrtab", stderr);
    }

    return File::Write(path, elf);
  }

  FileType file_type{kUnknown};

  NsoHeader header{};

  std::vector<u8> image;
  const Elf64_Dyn* dynamic{};
  const Elf64_Nhdr* note{};

  struct {
    u64 symtab;
    u64 rela;
    u64 relasz;
    u64 jmprel;
    u64 pltrelsz;
    u64 strtab;
    u64 strsz;
    u64 pltgot;
    u64 hash;
    u64 gnu_hash;
    u64 init;
    u64 fini;
    u64 init_array;
    u64 init_arraysz;
    u64 fini_array;
    u64 fini_arraysz;
  } dyn_info{};

  struct {
    u64 addr;
    u64 size;
  } plt_info;

  struct {
    u64 hdr_addr;
    u64 hdr_size;
    u64 frame_addr;
    u64 frame_size;
  } eh_info{};
};
const std::array<u8, 4> NsoFile::nso_magic{{'N', 'S', 'O', '0'}};
const std::array<u8, 4> NsoFile::nro_magic{{'N', 'R', 'O', '0'}};
const std::array<u8, 4> NsoFile::mod_magic{{'M', 'O', 'D', '0'}};

static bool NsoToElf(const fs::path& path, const char* elf_path, const char* uncompressed_path, bool verbose = false) {
  NsoFile nso;
  if (!nso.Load(path)) {
    return false;
  }
  printf("%s:\n", path.string().c_str());
  nso.Dump(verbose);
  if (verbose) {
    nso.DumpElfInfo();
  }

  bool success = true;
  if (elf_path)
    success &= nso.WriteElf(fs::path(elf_path));

  if (uncompressed_path)
    success &= nso.WriteUncompressedNso(fs::path(uncompressed_path));

  return success;
}

int main(int argc, char** argv) {
  const char* usage = "Usage: nx2elf <file or directory> [--export-uncompressed <path>] [--export-elf <path>]\n";

  if (argc < 2) {
    fputs(usage, stderr);
    return 1;
  }

  const char* input_path = nullptr;
  const char* elf_path = nullptr;
  const char* uncompressed_path = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--export-elf") == 0) {
      elf_path = argv[++i];
    } else if (strcmp(argv[i], "--export-uncompressed") == 0) {
      uncompressed_path = argv[++i];
    } else if (input_path == nullptr) {
      input_path = argv[i];
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      fputs(usage, stderr);
      return 1;
    }
  }

  fs::path path(input_path);
  if (fs::is_directory(path)) {
    File::iter_files(path, [elf_path, uncompressed_path](const fs::path& nx_path) { NsoToElf(nx_path, elf_path, uncompressed_path); });
  } else {
    NsoToElf(path, elf_path, uncompressed_path);
  }
  return 0;
}
