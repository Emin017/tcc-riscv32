/*
 * Mach-O file handling for TCC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "tcc.h"

/* In order to make life easy for us we are generating Mach-O files which
   don't make use of some modern features, but which aren't entirely classic
   either in that they do use some modern features.  We're also only
   generating 64bit Mach-O files, and only native endian at that.

   In particular we're generating executables that don't make use of
   DYLD_INFO for dynamic linking info, as that requires us building a
   trie of exported names.  We're simply using classic symbol tables which
   are still supported by modern dyld.

   But we do use LC_MAIN, which is a "modern" feature in order to not have
   to setup our own crt code.  We're not using lazy linking, so even function
   calls are resolved at startup.  */

#if !defined TCC_TARGET_X86_64 && !defined TCC_TARGET_ARM64
#error Platform not supported
#endif

#define DEBUG_MACHO 0
#define dprintf if (DEBUG_MACHO) printf

#define MH_EXECUTE              (0x2)
#define MH_DYLDLINK             (0x4)
#define MH_PIE                  (0x200000)

#define CPU_SUBTYPE_LIB64       (0x80000000)
#define CPU_SUBTYPE_X86_ALL     (3)
#define CPU_SUBTYPE_ARM64_ALL   (0)

#define CPU_ARCH_ABI64          (0x01000000)

#define CPU_TYPE_X86            (7)
#define CPU_TYPE_X86_64         (CPU_TYPE_X86 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM            (12)
#define CPU_TYPE_ARM64          (CPU_TYPE_ARM | CPU_ARCH_ABI64)

struct fat_header {
    uint32_t        magic;          /* FAT_MAGIC or FAT_MAGIC_64 */
    uint32_t        nfat_arch;      /* number of structs that follow */
};

struct fat_arch {
    int             cputype;        /* cpu specifier (int) */
    int             cpusubtype;     /* machine specifier (int) */
    uint32_t        offset;         /* file offset to this object file */
    uint32_t        size;           /* size of this object file */
    uint32_t        align;          /* alignment as a power of 2 */
};

#define FAT_MAGIC       0xcafebabe
#define FAT_CIGAM       0xbebafeca
#define FAT_MAGIC_64    0xcafebabf
#define FAT_CIGAM_64    0xbfbafeca

struct mach_header {
    uint32_t        magic;          /* mach magic number identifier */
    int             cputype;        /* cpu specifier */
    int             cpusubtype;     /* machine specifier */
    uint32_t        filetype;       /* type of file */
    uint32_t        ncmds;          /* number of load commands */
    uint32_t        sizeofcmds;     /* the size of all the load commands */
    uint32_t        flags;          /* flags */
};

struct mach_header_64 {
    struct mach_header  mh;
    uint32_t            reserved;       /* reserved, pad to 64bit */
};

/* Constant for the magic field of the mach_header (32-bit architectures) */
#define MH_MAGIC        0xfeedface      /* the mach magic number */
#define MH_CIGAM        0xcefaedfe      /* NXSwapInt(MH_MAGIC) */
#define MH_MAGIC_64     0xfeedfacf      /* the 64-bit mach magic number */
#define MH_CIGAM_64     0xcffaedfe      /* NXSwapInt(MH_MAGIC_64) */

struct load_command {
    uint32_t        cmd;            /* type of load command */
    uint32_t        cmdsize;        /* total size of command in bytes */
};

#define LC_REQ_DYLD 0x80000000
#define LC_SYMTAB        0x2
#define LC_DYSYMTAB      0xb
#define LC_LOAD_DYLIB    0xc
#define LC_ID_DYLIB      0xd
#define LC_LOAD_DYLINKER 0xe
#define LC_SEGMENT_64    0x19
#define LC_REEXPORT_DYLIB (0x1f | LC_REQ_DYLD)
#define LC_DYLD_INFO_ONLY (0x22|LC_REQ_DYLD)
#define LC_MAIN (0x28|LC_REQ_DYLD)

typedef int vm_prot_t;

struct segment_command_64 { /* for 64-bit architectures */
    uint32_t        cmd;            /* LC_SEGMENT_64 */
    uint32_t        cmdsize;        /* includes sizeof section_64 structs */
    char            segname[16];    /* segment name */
    uint64_t        vmaddr;         /* memory address of this segment */
    uint64_t        vmsize;         /* memory size of this segment */
    uint64_t        fileoff;        /* file offset of this segment */
    uint64_t        filesize;       /* amount to map from the file */
    vm_prot_t       maxprot;        /* maximum VM protection */
    vm_prot_t       initprot;       /* initial VM protection */
    uint32_t        nsects;         /* number of sections in segment */
    uint32_t        flags;          /* flags */
};

struct section_64 { /* for 64-bit architectures */
    char            sectname[16];   /* name of this section */
    char            segname[16];    /* segment this section goes in */
    uint64_t        addr;           /* memory address of this section */
    uint64_t        size;           /* size in bytes of this section */
    uint32_t        offset;         /* file offset of this section */
    uint32_t        align;          /* section alignment (power of 2) */
    uint32_t        reloff;         /* file offset of relocation entries */
    uint32_t        nreloc;         /* number of relocation entries */
    uint32_t        flags;          /* flags (section type and attributes)*/
    uint32_t        reserved1;      /* reserved (for offset or index) */
    uint32_t        reserved2;      /* reserved (for count or sizeof) */
    uint32_t        reserved3;      /* reserved */
};

#define S_REGULAR                       0x0
#define S_ZEROFILL                      0x1
#define S_NON_LAZY_SYMBOL_POINTERS      0x6
#define S_LAZY_SYMBOL_POINTERS          0x7
#define S_SYMBOL_STUBS                  0x8
#define S_MOD_INIT_FUNC_POINTERS        0x9
#define S_MOD_TERM_FUNC_POINTERS        0xa

#define S_ATTR_PURE_INSTRUCTIONS        0x80000000
#define S_ATTR_SOME_INSTRUCTIONS        0x00000400

typedef uint32_t lc_str;

struct dylib_command {
    uint32_t cmd;                   /* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB,
                                       LC_REEXPORT_DYLIB */
    uint32_t cmdsize;               /* includes pathname string */
    lc_str   name;                  /* library's path name */
    uint32_t timestamp;             /* library's build time stamp */
    uint32_t current_version;       /* library's current version number */
    uint32_t compatibility_version; /* library's compatibility vers number*/
};

struct dylinker_command {
    uint32_t        cmd;            /* LC_ID_DYLINKER, LC_LOAD_DYLINKER or
                                       LC_DYLD_ENVIRONMENT */
    uint32_t        cmdsize;        /* includes pathname string */
    lc_str          name;           /* dynamic linker's path name */
};

struct symtab_command {
    uint32_t        cmd;            /* LC_SYMTAB */
    uint32_t        cmdsize;        /* sizeof(struct symtab_command) */
    uint32_t        symoff;         /* symbol table offset */
    uint32_t        nsyms;          /* number of symbol table entries */
    uint32_t        stroff;         /* string table offset */
    uint32_t        strsize;        /* string table size in bytes */
};

struct dysymtab_command {
    uint32_t cmd;       /* LC_DYSYMTAB */
    uint32_t cmdsize;   /* sizeof(struct dysymtab_command) */

    uint32_t ilocalsym; /* index to local symbols */
    uint32_t nlocalsym; /* number of local symbols */

    uint32_t iextdefsym;/* index to externally defined symbols */
    uint32_t nextdefsym;/* number of externally defined symbols */

    uint32_t iundefsym; /* index to undefined symbols */
    uint32_t nundefsym; /* number of undefined symbols */

    uint32_t tocoff;    /* file offset to table of contents */
    uint32_t ntoc;      /* number of entries in table of contents */

    uint32_t modtaboff; /* file offset to module table */
    uint32_t nmodtab;   /* number of module table entries */

    uint32_t extrefsymoff;  /* offset to referenced symbol table */
    uint32_t nextrefsyms;   /* number of referenced symbol table entries */

    uint32_t indirectsymoff;/* file offset to the indirect symbol table */
    uint32_t nindirectsyms; /* number of indirect symbol table entries */

    uint32_t extreloff; /* offset to external relocation entries */
    uint32_t nextrel;   /* number of external relocation entries */
    uint32_t locreloff; /* offset to local relocation entries */
    uint32_t nlocrel;   /* number of local relocation entries */
};

#define BIND_OPCODE_DONE                                        0x00
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM                       0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM               0x40
#define BIND_OPCODE_SET_TYPE_IMM                                0x50
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB                 0x70
#define BIND_OPCODE_DO_BIND                                     0x90

#define BIND_TYPE_POINTER                                       1
#define BIND_SPECIAL_DYLIB_FLAT_LOOKUP                          -2

#define REBASE_OPCODE_DONE                                      0x00
#define REBASE_OPCODE_SET_TYPE_IMM                              0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB               0x20
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES                       0x50

#define REBASE_TYPE_POINTER                                     1

struct dyld_info_command {
    uint32_t cmd;             /* LC_DYLD_INFO or LC_DYLD_INFO_ONLY */
    uint32_t cmdsize;         /* sizeof(struct dyld_info_command) */
    uint32_t rebase_off;      /* file offset to rebase info  */
    uint32_t rebase_size;     /* size of rebase info   */
    uint32_t bind_off;        /* file offset to binding info   */
    uint32_t bind_size;       /* size of binding info  */
    uint32_t weak_bind_off;   /* file offset to weak binding info   */
    uint32_t weak_bind_size;  /* size of weak binding info  */
    uint32_t lazy_bind_off;   /* file offset to lazy binding info */
    uint32_t lazy_bind_size;  /* size of lazy binding infs */
    uint32_t export_off;      /* file offset to lazy binding info */
    uint32_t export_size;     /* size of lazy binding infs */
};

#define INDIRECT_SYMBOL_LOCAL   0x80000000

struct entry_point_command {
    uint32_t  cmd;      /* LC_MAIN only used in MH_EXECUTE filetypes */
    uint32_t  cmdsize;  /* 24 */
    uint64_t  entryoff; /* file (__TEXT) offset of main() */
    uint64_t  stacksize;/* if not zero, initial stack size */
};

enum skind {
    sk_unknown = 0,
    sk_discard,
    sk_text,
    sk_stubs,
    sk_stub_helper,
    sk_ro_data,
    sk_uw_info,
    sk_nl_ptr,  // non-lazy pointers, aka GOT
    sk_la_ptr,  // lazy pointers
    sk_init,
    sk_fini,
    sk_rw_data,
    sk_stab,
    sk_stab_str,
    sk_debug_info,
    sk_debug_abbrev,
    sk_debug_line,
    sk_debug_aranges,
    sk_debug_str,
    sk_debug_line_str,
    sk_bss,
    sk_linkedit,
    sk_last
};

struct nlist_64 {
    uint32_t  n_strx;      /* index into the string table */
    uint8_t n_type;        /* type flag, see below */
    uint8_t n_sect;        /* section number or NO_SECT */
    uint16_t n_desc;       /* see <mach-o/stab.h> */
    uint64_t n_value;      /* value of this symbol (or stab offset) */
};

#define N_UNDF  0x0
#define N_ABS   0x2
#define N_EXT   0x1
#define N_SECT  0xe

#define N_WEAK_REF      0x0040
#define N_WEAK_DEF      0x0080

struct macho {
    struct mach_header_64 mh;
    int seg2lc[4], nseg;
    struct load_command **lc;
    struct entry_point_command *ep;
    int nlc;
    struct {
        Section *s;
        int machosect;
    } sk_to_sect[sk_last];
    int *elfsectomacho;
    int *e2msym;
    Section *rebase, *binding, *weak_binding, *lazy_binding, *exports;
    Section *symtab, *strtab, *wdata, *indirsyms;
    Section *stubs, *stub_helper, *la_symbol_ptr;
    int nr_plt, n_got;
    struct dyld_info_command *dyldinfo;
    int stubsym, helpsym, lasym, dyld_private, dyld_stub_binder;
    uint32_t ilocal, iextdef, iundef;
    int n_lazy_bind_rebase;    
    struct lazy_bind_rebase {
        int section;
        int bind;
        int bind_offset;
        int la_symbol_offset;
        ElfW_Rel rel;
    } *lazy_bind_rebase;
    int n_bind; 
    struct bind {
        int section;
        ElfW_Rel rel;
    } *bind;
};

#define SHT_LINKEDIT (SHT_LOOS + 42)
#define SHN_FROMDLL  (SHN_LOOS + 2)  /* Symbol is undefined, comes from a DLL */

static void * add_lc(struct macho *mo, uint32_t cmd, uint32_t cmdsize)
{
    struct load_command *lc = tcc_mallocz(cmdsize);
    lc->cmd = cmd;
    lc->cmdsize = cmdsize;
    mo->lc = tcc_realloc(mo->lc, sizeof(mo->lc[0]) * (mo->nlc + 1));
    mo->lc[mo->nlc++] = lc;
    return lc;
}

static struct segment_command_64 * add_segment(struct macho *mo, const char *name)
{
    struct segment_command_64 *sc = add_lc(mo, LC_SEGMENT_64, sizeof(*sc));
    strncpy(sc->segname, name, 16);
    mo->seg2lc[mo->nseg++] = mo->nlc - 1;
    return sc;
}

static struct segment_command_64 * get_segment(struct macho *mo, int i)
{
    return (struct segment_command_64 *) (mo->lc[mo->seg2lc[i]]);
}

static int add_section(struct macho *mo, struct segment_command_64 **_seg, const char *name)
{
    struct segment_command_64 *seg = *_seg;
    int ret = seg->nsects;
    struct section_64 *sec;
    seg->nsects++;
    seg->cmdsize += sizeof(*sec);
    seg = tcc_realloc(seg, sizeof(*seg) + seg->nsects * sizeof(*sec));
    sec = (struct section_64*)((char*)seg + sizeof(*seg)) + ret;
    memset(sec, 0, sizeof(*sec));
    strncpy(sec->sectname, name, 16);
    strncpy(sec->segname, seg->segname, 16);
    *_seg = seg;
    return ret;
}

static struct section_64 *get_section(struct segment_command_64 *seg, int i)
{
    return (struct section_64*)((char*)seg + sizeof(*seg)) + i;
}

static void * add_dylib(struct macho *mo, char *name)
{
    struct dylib_command *lc;
    int sz = (sizeof(*lc) + strlen(name) + 1 + 7) & -8;
    lc = add_lc(mo, LC_LOAD_DYLIB, sz);
    lc->name = sizeof(*lc);
    strcpy((char*)lc + lc->name, name);
    lc->timestamp = 2;
    lc->current_version = 1 << 16;
    lc->compatibility_version = 1 << 16;
    return lc;
}

static void write_uleb128(Section *section, uint64_t value)
{
    do {
        unsigned char byte = value & 0x7f;
	uint8_t *ptr = section_ptr_add(section, 1);

        value >>= 7;
        *ptr = byte | (value ? 0x80 : 0);
    } while (value != 0);
}

static void tcc_macho_add_destructor(TCCState *s1)
{
    int init_sym, mh_execute_header, at_exit_sym;
    Section *s;
    ElfW_Rel *rel;
    uint8_t *ptr;

    s = find_section(s1, ".fini_array");
    if (s->data_offset == 0)
        return; 
    init_sym = put_elf_sym(s1->symtab, text_section->data_offset, 0,
                           ELFW(ST_INFO)(STB_LOCAL, STT_FUNC), 0,
                           text_section->sh_num, "___GLOBAL_init_65535");
    mh_execute_header = put_elf_sym(s1->symtab, 0x100000000ll, 0,
                      		    ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0,
                      		    SHN_ABS, "__mh_execute_header");
    at_exit_sym = put_elf_sym(s1->symtab, 0, 0,
                              ELFW(ST_INFO)(STB_GLOBAL, STT_FUNC), 0,
                              SHN_UNDEF, "___cxa_atexit");
#ifdef TCC_TARGET_X86_64
    ptr = section_ptr_add(text_section, 4);
    ptr[0] = 0x55;  // pushq   %rbp
    ptr[1] = 0x48;  // movq    %rsp, %rbp
    ptr[2] = 0x89;
    ptr[3] = 0xe5;
    for_each_elem(s->reloc, 0, rel, ElfW_Rel) {
        int sym_index = ELFW(R_SYM)(rel->r_info);

        ptr = section_ptr_add(text_section, 26);
        ptr[0] = 0x48;  // lea destructor(%rip),%rax
        ptr[1] = 0x8d;
        ptr[2] = 0x05;
        put_elf_reloca(s1->symtab, text_section, 
		       text_section->data_offset - 23,
		       R_X86_64_PC32, sym_index, -4);
        ptr[7] = 0x48;  // mov %rax,%rdi
        ptr[8] = 0x89;
        ptr[9] = 0xc7;
	ptr[10] = 0x31; // xorl %ecx, %ecx
	ptr[11] = 0xc9;
	ptr[12] = 0x89; // movl %ecx, %esi
	ptr[13] = 0xce;
        ptr[14] = 0x48;  // lea mh_execute_header(%rip),%rdx
        ptr[15] = 0x8d;
        ptr[16] = 0x15;
        put_elf_reloca(s1->symtab, text_section,
		       text_section->data_offset - 9,
		       R_X86_64_PC32, mh_execute_header, -4);
	ptr[21] = 0xe8; // call __cxa_atexit
        put_elf_reloca(s1->symtab, text_section,
		       text_section->data_offset - 4,
		       R_X86_64_PLT32, at_exit_sym, -4);
    }
    ptr = section_ptr_add(text_section, 2);
    ptr[0] = 0x5d;  // pop   %rbp
    ptr[1] = 0xc3;  // ret
#elif defined TCC_TARGET_ARM64
    ptr = section_ptr_add(text_section, 8);
    write32le(ptr, 0xa9bf7bfd);     // stp     x29, x30, [sp, #-16]!
    write32le(ptr + 4, 0x910003fd); // mov     x29, sp
    for_each_elem(s->reloc, 0, rel, ElfW_Rel) {
        int sym_index = ELFW(R_SYM)(rel->r_info);

        ptr = section_ptr_add(text_section, 24);
        put_elf_reloc(s1->symtab, text_section, 
		      text_section->data_offset - 24,
		      R_AARCH64_ADR_PREL_PG_HI21, sym_index);
        write32le(ptr, 0x90000000);      // adrp x0, destructor@page
        put_elf_reloc(s1->symtab, text_section,
		      text_section->data_offset - 20,
		      R_AARCH64_LDST8_ABS_LO12_NC, sym_index);
        write32le(ptr + 4, 0x91000000);  // add x0,x0,destructor@pageoff
        write32le(ptr + 8, 0xd2800001);  // mov x1, #0
        put_elf_reloc(s1->symtab, text_section, 
		      text_section->data_offset - 12,
		      R_AARCH64_ADR_PREL_PG_HI21, mh_execute_header);
        write32le(ptr + 12, 0x90000002);      // adrp x2, mh_execute_header@page
        put_elf_reloc(s1->symtab, text_section,
		      text_section->data_offset - 8,
		      R_AARCH64_LDST8_ABS_LO12_NC, mh_execute_header);
        write32le(ptr + 16, 0x91000042);  // add x2,x2,mh_execute_header@pageoff
        put_elf_reloc(s1->symtab, text_section,
		      text_section->data_offset - 4,
		      R_AARCH64_CALL26, at_exit_sym);
	write32le(ptr + 20, 0x94000000); // bl __cxa_atexit
    }
    ptr = section_ptr_add(text_section, 8);
    write32le(ptr, 0xa8c17bfd);     // ldp     x29, x30, [sp], #16
    write32le(ptr + 4, 0xd65f03c0); // ret
#endif
    s->reloc->data_offset = s->data_offset = 0;
    s->sh_flags &= ~SHF_ALLOC;
    add_array (s1, ".init_array", init_sym);
}

static void check_relocs(TCCState *s1, struct macho *mo)
{
    uint8_t *jmp;
    Section *s;
    ElfW_Rel *rel;
    ElfW(Sym) *sym;
    int i, type, gotplt_entry, sym_index, for_code;
    int sh_num, debug, bind_offset, la_symbol_offset;
    uint32_t *pi, *goti;
    struct sym_attr *attr;

    mo->indirsyms = new_section(s1, "LEINDIR", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);

#ifdef TCC_TARGET_X86_64
    jmp = section_ptr_add(mo->stub_helper, 16);
    jmp[0] = 0x4c;  /* leaq _dyld_private(%rip), %r11 */
    jmp[1] = 0x8d;
    jmp[2] = 0x1d;
    put_elf_reloca(s1->symtab, mo->stub_helper, 3,
		   R_X86_64_PC32, mo->dyld_private, -4);
    jmp[7] = 0x41;  /* pushq %r11 */
    jmp[8] = 0x53;
    jmp[9] = 0xff;  /* jmpq    *dyld_stub_binder@GOT(%rip) */
    jmp[10] = 0x25;
    put_elf_reloca(s1->symtab, mo->stub_helper, 11,
		   R_X86_64_GOTPCREL, mo->dyld_stub_binder, -4);
    jmp[15] = 0x90; /* nop */
#elif defined TCC_TARGET_ARM64
    jmp = section_ptr_add(mo->stub_helper, 24);
    put_elf_reloc(s1->symtab, mo->stub_helper, 0,
		  R_AARCH64_ADR_PREL_PG_HI21, mo->dyld_private);
    write32le(jmp, 0x90000011); // adrp x17, _dyld_private@page
    put_elf_reloc(s1->symtab, mo->stub_helper, 4,
		  R_AARCH64_LDST64_ABS_LO12_NC, mo->dyld_private);
    write32le(jmp + 4, 0x91000231); // add x17,x17,_dyld_private@pageoff
    write32le(jmp + 8, 0xa9bf47f0); // stp x16/x17, [sp, #-16]!
    put_elf_reloc(s1->symtab, mo->stub_helper, 12,
		  R_AARCH64_ADR_GOT_PAGE, mo->dyld_stub_binder);
    write32le(jmp + 12, 0x90000010); // adrp x16, dyld_stub_binder@page
    put_elf_reloc(s1->symtab, mo->stub_helper, 16,
		  R_AARCH64_LD64_GOT_LO12_NC, mo->dyld_stub_binder);
    write32le(jmp + 16, 0xf9400210); // ldr x16,[x16,dyld_stub_binder@pageoff]
    write32le(jmp + 20, 0xd61f0200); // br x16
#endif
    
    goti = NULL;
    mo->nr_plt = mo->n_got = 0;
    for (i = 1; i < s1->nb_sections; i++) {
        s = s1->sections[i];
        if (s->sh_type != SHT_RELX)
            continue;
	sh_num = s1->sections[s->sh_info]->sh_num;
	debug = sh_num >= s1->dwlo && sh_num < s1->dwhi;
        for_each_elem(s, 0, rel, ElfW_Rel) {
            type = ELFW(R_TYPE)(rel->r_info);
            gotplt_entry = gotplt_entry_type(type);
            for_code = code_reloc(type);
            /* We generate a non-lazy pointer for used undefined symbols
               and for defined symbols that must have a place for their
               address due to codegen (i.e. a reloc requiring a got slot).  */
            sym_index = ELFW(R_SYM)(rel->r_info);
            sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
            if (!debug &&
		(sym->st_shndx == SHN_UNDEF
                 || gotplt_entry == ALWAYS_GOTPLT_ENTRY)) {
                attr = get_sym_attr(s1, sym_index, 1);
                if (!attr->dyn_index) {
                    attr->got_offset = s1->got->data_offset;
                    attr->plt_offset = -1;
                    attr->dyn_index = 1; /* used as flag */
		    section_ptr_add(s1->got, PTR_SIZE);
                    put_elf_reloc(s1->symtab, s1->got, attr->got_offset,
                                  R_DATA_PTR, sym_index);
		    goti = tcc_realloc(goti, (mo->n_got + 1) * sizeof(*goti));
                    if (ELFW(ST_BIND)(sym->st_info) == STB_LOCAL) {
                        if (sym->st_shndx == SHN_UNDEF)
                          tcc_error("undefined local symbo: '%s'",
				    (char *) symtab_section->link->data + sym->st_name);
			goti[mo->n_got++] = INDIRECT_SYMBOL_LOCAL;
                    } else {
			goti[mo->n_got++] = mo->e2msym[sym_index];
			if (sym->st_shndx == SHN_UNDEF
#ifdef TCC_TARGET_X86_64
			    && type == R_X86_64_GOTPCREL
#elif defined TCC_TARGET_ARM64
			    && type == R_AARCH64_ADR_GOT_PAGE
#endif
			    ) {
			    mo->bind =
			        tcc_realloc(mo->bind,
					    (mo->n_bind + 1) *
					    sizeof(struct bind));
			    mo->bind[mo->n_bind].section = s1->got->reloc->sh_info;
			    mo->bind[mo->n_bind].rel = *rel;
                            mo->bind[mo->n_bind].rel.r_offset = attr->got_offset;
			    mo->n_bind++;
			    s1->got->reloc->data_offset -= sizeof (ElfW_Rel);
			}
		    }
                }
                if (for_code && sym->st_shndx == SHN_UNDEF) {
                    if (attr->plt_offset == -1) {
			pi = section_ptr_add(mo->indirsyms, sizeof(*pi));
			*pi = mo->e2msym[sym_index];
			mo->nr_plt++;
                        attr->plt_offset = mo->stubs->data_offset;
#ifdef TCC_TARGET_X86_64
			if (type != R_X86_64_PLT32)
			     continue;
			/* __stubs */
                        jmp = section_ptr_add(mo->stubs, 6);
                        jmp[0] = 0xff;  /* jmpq *__la_symbol_ptr(%rip) */
                        jmp[1] = 0x25;
                        put_elf_reloca(s1->symtab, mo->stubs,
                                       mo->stubs->data_offset - 4,
                                       R_X86_64_PC32, mo->lasym,
				       mo->la_symbol_ptr->data_offset - 4);

			/* __stub_helper */
			bind_offset = mo->stub_helper->data_offset + 1;
                        jmp = section_ptr_add(mo->stub_helper, 10);
                        jmp[0] = 0x68;  /* pushq $bind_offset */
                        jmp[5] = 0xe9;  /* jmpq __stub_helper */
                        write32le(jmp + 6, -mo->stub_helper->data_offset);

			/* __la_symbol_ptr */
			la_symbol_offset = mo->la_symbol_ptr->data_offset;
                        put_elf_reloca(s1->symtab, mo->la_symbol_ptr,
				       mo->la_symbol_ptr->data_offset,
				       R_DATA_PTR, mo->helpsym,
				       mo->stub_helper->data_offset - 10);
			jmp = section_ptr_add(mo->la_symbol_ptr, PTR_SIZE);
#elif defined TCC_TARGET_ARM64
			if (type != R_AARCH64_CALL26)
			     continue;
			/* __stubs */
                        jmp = section_ptr_add(mo->stubs, 12);
                        put_elf_reloca(s1->symtab, mo->stubs,
                                       mo->stubs->data_offset - 12,
                                       R_AARCH64_ADR_PREL_PG_HI21, mo->lasym,
				       mo->la_symbol_ptr->data_offset);
                        write32le(jmp, // adrp x16, __la_symbol_ptr@page
                                  0x90000010);
                        put_elf_reloca(s1->symtab, mo->stubs,
                                       mo->stubs->data_offset - 8,
                                       R_AARCH64_LDST64_ABS_LO12_NC, mo->lasym,
				       mo->la_symbol_ptr->data_offset);
                        write32le(jmp + 4, // ldr x16,[x16, __la_symbol_ptr@pageoff]
                                  0xf9400210);
                        write32le(jmp + 8, // br x16
                                  0xd61f0200);

			/* __stub_helper */
			bind_offset = mo->stub_helper->data_offset + 8;
                        jmp = section_ptr_add(mo->stub_helper, 12);
                        write32le(jmp + 0, // ldr  w16, l0
                                  0x18000050);
                        write32le(jmp + 4, // b stubHelperHeader
                                  0x14000000 +
				  ((-(mo->stub_helper->data_offset - 8) / 4) &
				   0x3ffffff));
                        write32le(jmp + 8, 0); // l0: .long bind_offset

			/* __la_symbol_ptr */
			la_symbol_offset = mo->la_symbol_ptr->data_offset;
                        put_elf_reloca(s1->symtab, mo->la_symbol_ptr,
				       mo->la_symbol_ptr->data_offset,
				       R_DATA_PTR, mo->helpsym,
				       mo->stub_helper->data_offset - 12);
			jmp = section_ptr_add(mo->la_symbol_ptr, PTR_SIZE);
#endif
                        mo->lazy_bind_rebase =
                            tcc_realloc(mo->lazy_bind_rebase,
                                        (mo->n_lazy_bind_rebase + 1) *
                                        sizeof(struct lazy_bind_rebase));
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].section =
                            mo->stub_helper->reloc->sh_info;
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].bind = 1;
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].bind_offset = bind_offset;
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].la_symbol_offset = la_symbol_offset;
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].rel = *rel;
                        mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].rel.r_offset =
                            attr->plt_offset;
                        mo->n_lazy_bind_rebase++;
                    }
                    rel->r_info = ELFW(R_INFO)(mo->stubsym, type);
                    rel->r_addend += attr->plt_offset;
                }
            }
            if (type == R_DATA_PTR) {
                mo->lazy_bind_rebase =
                    tcc_realloc(mo->lazy_bind_rebase,
                                (mo->n_lazy_bind_rebase + 1) *
                                sizeof(struct lazy_bind_rebase));
                mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].section = s->sh_info;
                mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].bind = 0;
                mo->lazy_bind_rebase[mo->n_lazy_bind_rebase].rel = *rel;
                mo->n_lazy_bind_rebase++;
	    }
        }
    }
    pi = section_ptr_add(mo->indirsyms, mo->n_got * sizeof(*pi));
    memcpy(pi, goti, mo->n_got * sizeof(*pi));
    pi = section_ptr_add(mo->indirsyms, mo->nr_plt * sizeof(*pi));
    memcpy(pi, mo->indirsyms->data, mo->nr_plt * sizeof(*pi));
    tcc_free(goti);
}

static int check_symbols(TCCState *s1, struct macho *mo)
{
    int sym_index, sym_end;
    int ret = 0;

    mo->ilocal = mo->iextdef = mo->iundef = -1;
    sym_end = symtab_section->data_offset / sizeof(ElfW(Sym));
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        int elf_index = ((struct nlist_64 *)mo->symtab->data + sym_index - 1)->n_value;
        ElfW(Sym) *sym = (ElfW(Sym) *)symtab_section->data + elf_index;
        const char *name = (char*)symtab_section->link->data + sym->st_name;
        unsigned type = ELFW(ST_TYPE)(sym->st_info);
        unsigned bind = ELFW(ST_BIND)(sym->st_info);
        unsigned vis  = ELFW(ST_VISIBILITY)(sym->st_other);

        dprintf("%4d (%4d): %09lx %4d %4d %4d %3d %s\n",
                sym_index, elf_index, (long)sym->st_value,
                type, bind, vis, sym->st_shndx, name);
        if (bind == STB_LOCAL) {
            if (mo->ilocal == -1)
              mo->ilocal = sym_index - 1;
            if (mo->iextdef != -1 || mo->iundef != -1)
              tcc_error("local syms after global ones");
        } else if (sym->st_shndx != SHN_UNDEF) {
            if (mo->iextdef == -1)
              mo->iextdef = sym_index - 1;
            if (mo->iundef != -1)
              tcc_error("external defined symbol after undefined");
        } else if (sym->st_shndx == SHN_UNDEF) {
            if (mo->iundef == -1)
              mo->iundef = sym_index - 1;
            if (ELFW(ST_BIND)(sym->st_info) == STB_WEAK
                || find_elf_sym(s1->dynsymtab_section, name)) {
                /* Mark the symbol as coming from a dylib so that
                   relocate_syms doesn't complain.  Normally bind_exe_dynsyms
                   would do this check, and place the symbol into dynsym
                   which is checked by relocate_syms.  But Mach-O doesn't use
                   bind_exe_dynsyms.  */
                sym->st_shndx = SHN_FROMDLL;
                continue;
            }
            tcc_error_noabort("undefined symbol '%s'", name);
            ret = -1;
        }
    }
    return ret;
}

static void convert_symbol(TCCState *s1, struct macho *mo, struct nlist_64 *pn)
{
    struct nlist_64 n = *pn;
    ElfSym *sym = (ElfW(Sym) *)symtab_section->data + pn->n_value;
    const char *name = (char*)symtab_section->link->data + sym->st_name;
    switch(ELFW(ST_TYPE)(sym->st_info)) {
    case STT_NOTYPE:
    case STT_OBJECT:
    case STT_FUNC:
    case STT_SECTION:
        n.n_type = N_SECT;
        break;
    case STT_FILE:
        n.n_type = N_ABS;
        break;
    default:
        tcc_error("unhandled ELF symbol type %d %s",
                  ELFW(ST_TYPE)(sym->st_info), name);
    }
    if (sym->st_shndx == SHN_UNDEF)
      tcc_error("should have been rewritten to SHN_FROMDLL: %s", name);
    else if (sym->st_shndx == SHN_FROMDLL)
      n.n_type = N_UNDF, n.n_sect = 0;
    else if (sym->st_shndx == SHN_ABS)
      n.n_type = N_ABS, n.n_sect = 0;
    else if (sym->st_shndx >= SHN_LORESERVE)
      tcc_error("unhandled ELF symbol section %d %s", sym->st_shndx, name);
    else if (!mo->elfsectomacho[sym->st_shndx]) {
      int sh_num = s1->sections[sym->st_shndx]->sh_num;
      if (sh_num < s1->dwlo || sh_num >= s1->dwhi) 
        tcc_error("ELF section %d(%s) not mapped into Mach-O for symbol %s",
                  sym->st_shndx, s1->sections[sym->st_shndx]->name, name);
    }
    else
      n.n_sect = mo->elfsectomacho[sym->st_shndx];
    if (ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL)
      n.n_type |=  N_EXT;
    else if (ELFW(ST_BIND)(sym->st_info) == STB_WEAK)
      n.n_desc |= N_WEAK_REF | (n.n_type != N_UNDF ? N_WEAK_DEF : 0);
    n.n_strx = pn->n_strx;
    n.n_value = sym->st_value;
    *pn = n;
}

static void convert_symbols(TCCState *s1, struct macho *mo)
{
    struct nlist_64 *pn;
    for_each_elem(mo->symtab, 0, pn, struct nlist_64)
        convert_symbol(s1, mo, pn);
}

static int machosymcmp(const void *_a, const void *_b, void *arg)
{
    TCCState *s1 = arg;
    int ea = ((struct nlist_64 *)_a)->n_value;
    int eb = ((struct nlist_64 *)_b)->n_value;
    ElfSym *sa = (ElfSym *)symtab_section->data + ea;
    ElfSym *sb = (ElfSym *)symtab_section->data + eb;
    int r;
    /* locals, then defined externals, then undefined externals, the
       last two sections also by name, otherwise stable sort */
    r = (ELFW(ST_BIND)(sb->st_info) == STB_LOCAL)
        - (ELFW(ST_BIND)(sa->st_info) == STB_LOCAL);
    if (r)
      return r;
    r = (sa->st_shndx == SHN_UNDEF) - (sb->st_shndx == SHN_UNDEF);
    if (r)
      return r;
    if (ELFW(ST_BIND)(sa->st_info) != STB_LOCAL) {
        const char * na = (char*)symtab_section->link->data + sa->st_name;
        const char * nb = (char*)symtab_section->link->data + sb->st_name;
        r = strcmp(na, nb);
        if (r)
          return r;
    }
    return ea - eb;
}

/* cannot use qsort because code has to be reentrant */
static void tcc_qsort (void  *base, size_t nel, size_t width,
                       int (*comp)(const void *, const void *, void *), void *arg)
{
    size_t wnel, gap, wgap, i, j, k;
    char *a, *b, tmp;

    wnel = width * nel;
    for (gap = 0; ++gap < nel;)
        gap *= 3;
    while ( gap /= 3 ) {
        wgap = width * gap;
        for (i = wgap; i < wnel; i += width) {
            for (j = i - wgap; ;j -= wgap) {
                a = j + (char *)base;
                b = a + wgap;
                if ( (*comp)(a, b, arg) <= 0 )
                    break;
                k = width;
                do {
                    tmp = *a;
                    *a++ = *b;
                    *b++ = tmp;
                } while ( --k );
                if (j < wgap)
                    break;
            }
        }
    }
}

static void create_symtab(TCCState *s1, struct macho *mo)
{
    int sym_index, sym_end;
    struct nlist_64 *pn;

    /* Stub creation belongs to check_relocs, but we need to create
       the symbol now, so its included in the sorting.  */
    mo->stubs = new_section(s1, "__stubs", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    mo->stub_helper = new_section(s1, "__stub_helper", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR);
    s1->got = new_section(s1, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    mo->la_symbol_ptr = new_section(s1, "__la_symbol_ptr", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE);
    mo->stubsym = put_elf_sym(s1->symtab, 0, 0,
                              ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0,
                              mo->stubs->sh_num, ".__stubs");
    mo->helpsym = put_elf_sym(s1->symtab, 0, 0,
                              ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0,
                              mo->stub_helper->sh_num, ".__stub_helper");
    mo->lasym = put_elf_sym(s1->symtab, 0, 0,
                            ELFW(ST_INFO)(STB_LOCAL, STT_SECTION), 0,
                            mo->la_symbol_ptr->sh_num, ".__la_symbol_ptr");
    section_ptr_add(data_section, -data_section->data_offset & (PTR_SIZE - 1));
    mo->dyld_private = put_elf_sym(s1->symtab, data_section->data_offset, PTR_SIZE,
                                   ELFW(ST_INFO)(STB_LOCAL, STT_OBJECT), 0,
                                   data_section->sh_num, ".__dyld_private");
    section_ptr_add(data_section, PTR_SIZE);
    mo->dyld_stub_binder = put_elf_sym(s1->symtab, 0, 0,
                		       ELFW(ST_INFO)(STB_GLOBAL, STT_OBJECT), 0,
				       SHN_UNDEF, "dyld_stub_binder");
    mo->rebase = new_section(s1, "REBASE", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->binding = new_section(s1, "BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->weak_binding = new_section(s1, "WEAK_BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->lazy_binding = new_section(s1, "LAZY_BINDING", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->exports = new_section(s1, "EXPORT", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);

    mo->symtab = new_section(s1, "LESYMTAB", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    mo->strtab = new_section(s1, "LESTRTAB", SHT_LINKEDIT, SHF_ALLOC | SHF_WRITE);
    put_elf_str(mo->strtab, " "); /* Mach-O starts strtab with a space */
    sym_end = symtab_section->data_offset / sizeof(ElfW(Sym));
    pn = section_ptr_add(mo->symtab, sizeof(*pn) * (sym_end - 1));
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        ElfW(Sym) *sym = (ElfW(Sym) *)symtab_section->data + sym_index;
        const char *name = (char*)symtab_section->link->data + sym->st_name;
        pn[sym_index - 1].n_strx = put_elf_str(mo->strtab, name);
        pn[sym_index - 1].n_value = sym_index;
    }
    tcc_qsort(pn, sym_end - 1, sizeof(*pn), machosymcmp, s1);
    mo->e2msym = tcc_malloc(sym_end * sizeof(*mo->e2msym));
    mo->e2msym[0] = -1;
    for (sym_index = 1; sym_index < sym_end; ++sym_index) {
        mo->e2msym[pn[sym_index - 1].n_value] = sym_index - 1;
    }
}

const struct {
    int seg;
    uint32_t flags;
    const char *name;
} skinfo[sk_last] = {
    /*[sk_unknown] =*/        { 0 },
    /*[sk_discard] =*/        { 0 },
    /*[sk_text] =*/           { 1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS
                                | S_ATTR_SOME_INSTRUCTIONS, "__text" },
    /*[sk_stubs] =*/          { 1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_SYMBOL_STUBS
                                   | S_ATTR_SOME_INSTRUCTIONS , "__stubs" },
    /*[sk_stub_helper] =*/    { 1, S_REGULAR | S_ATTR_PURE_INSTRUCTIONS
                                   | S_ATTR_SOME_INSTRUCTIONS , "__stub_helper" },
    /*[sk_ro_data] =*/        { 1, S_REGULAR, "__rodata" },
    /*[sk_uw_info] =*/        { 0 },
    /*[sk_nl_ptr] =*/           { 2, S_NON_LAZY_SYMBOL_POINTERS, "__got" },
    /*[sk_la_ptr] =*/         { 2, S_LAZY_SYMBOL_POINTERS, "__la_symbol_ptr" },
    /*[sk_init] =*/           { 2, S_MOD_INIT_FUNC_POINTERS, "__mod_init_func" },
    /*[sk_fini] =*/           { 2, S_MOD_TERM_FUNC_POINTERS, "__mod_term_func" },
    /*[sk_rw_data] =*/        { 2, S_REGULAR, "__data" },
    /*[sk_stab] =*/           { 2, S_REGULAR, "__stab" },
    /*[sk_stab_str] =*/       { 2, S_REGULAR, "__stab_str" },
    /*[sk_debug_info] =*/     { 2, S_REGULAR, "__debug_info" },
    /*[sk_debug_abbrev] =*/   { 2, S_REGULAR, "__debug_abbrev" },
    /*[sk_debug_line] =*/     { 2, S_REGULAR, "__debug_line" },
    /*[sk_debug_aranges] =*/  { 2, S_REGULAR, "__debug_aranges" },
    /*[sk_debug_str] =*/      { 2, S_REGULAR, "__debug_str" },
    /*[sk_debug_line_str] =*/ { 2, S_REGULAR, "__debug_line_str" },
    /*[sk_bss] =*/            { 2, S_ZEROFILL, "__bss" },
    /*[sk_linkedit] =*/       { 3, S_REGULAR, NULL },
};

static void set_segment_and_offset(struct macho *mo, addr_t addr,
				   uint8_t *ptr, int opcode,
				   Section *sec, addr_t offset)
{
    int i;
    struct segment_command_64 *seg = NULL;

    for (i = 1; i < mo->nseg - 1; i++) {
	seg = get_segment(mo, i);
	if (addr >= seg->vmaddr && addr < (seg->vmaddr + seg->vmsize))
	    break;
    }
    *ptr = opcode | i;
    write_uleb128(sec, offset - seg->vmaddr);
}

static void do_bind_rebase(TCCState *s1, struct macho *mo)
{
    int i;
    uint8_t *ptr;
    ElfW(Sym) *sym;
    char *name;

    for (i = 0; i < mo->n_lazy_bind_rebase; i++) {
	int sym_index = ELFW(R_SYM)(mo->lazy_bind_rebase[i].rel.r_info);
	Section *s = s1->sections[mo->lazy_bind_rebase[i].section];

	sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
	name = (char *) symtab_section->link->data + sym->st_name;
	if (mo->lazy_bind_rebase[i].bind) {
	    write32le(mo->stub_helper->data +
		      mo->lazy_bind_rebase[i].bind_offset,
		      mo->lazy_binding->data_offset);
	    ptr = section_ptr_add(mo->lazy_binding, 1);
	    set_segment_and_offset(mo, mo->la_symbol_ptr->sh_addr, ptr,
				   BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB,
				   mo->lazy_binding,
				   mo->la_symbol_ptr->sh_addr +
				   mo->lazy_bind_rebase[i].la_symbol_offset);
	    ptr = section_ptr_add(mo->lazy_binding, 5 + strlen(name));
	    *ptr++ = BIND_OPCODE_SET_DYLIB_SPECIAL_IMM |
		     (BIND_SPECIAL_DYLIB_FLAT_LOOKUP & 0xf);
	    *ptr++ = BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0;
	    strcpy(ptr, name);
	    ptr += strlen(name) + 1;
	    *ptr++ = BIND_OPCODE_DO_BIND;
	    *ptr = BIND_OPCODE_DONE;
	}
	else {
	    ptr = section_ptr_add(mo->rebase, 2);
	    *ptr++ = REBASE_OPCODE_SET_TYPE_IMM | REBASE_TYPE_POINTER;
	    set_segment_and_offset(mo, s->sh_addr, ptr,
				   REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB,
				   mo->rebase,
				   mo->lazy_bind_rebase[i].rel.r_offset +
				   s->sh_addr);
	    ptr = section_ptr_add(mo->rebase, 1);
	    *ptr = REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1;
	}
    }
    for (i = 0; i < mo->n_bind; i++) {
	int sym_index = ELFW(R_SYM)(mo->bind[i].rel.r_info);
	Section *s = s1->sections[mo->bind[i].section];
	Section *binding;

	sym = &((ElfW(Sym) *)symtab_section->data)[sym_index];
	name = (char *) symtab_section->link->data + sym->st_name;
	binding = ELFW(ST_BIND)(sym->st_info) == STB_WEAK
	    ? mo->weak_binding : mo->binding;
        ptr = section_ptr_add(binding, 5 + strlen(name));
        *ptr++ = BIND_OPCODE_SET_DYLIB_SPECIAL_IMM |
	         (BIND_SPECIAL_DYLIB_FLAT_LOOKUP & 0xf);
        *ptr++ = BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | 0;
        strcpy(ptr, name);
        ptr += strlen(name) + 1;
        *ptr++ = BIND_OPCODE_SET_TYPE_IMM | BIND_TYPE_POINTER;
	    set_segment_and_offset(mo, s->sh_addr, ptr,
				   BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB,
				   binding,
				   mo->bind[i].rel.r_offset + s->sh_addr);
        ptr = section_ptr_add(binding, 1);
        *ptr++ = BIND_OPCODE_DO_BIND;
    }
    if (mo->rebase->data_offset) {
        ptr = section_ptr_add(mo->rebase, 1);
        *ptr = REBASE_OPCODE_DONE;
    }
    tcc_free(mo->lazy_bind_rebase);
    tcc_free(mo->bind);
}

static void collect_sections(TCCState *s1, struct macho *mo)
{
    int i, sk, numsec;
    uint64_t curaddr, fileofs;
    Section *s;
    struct segment_command_64 *seg = NULL;
    struct dylinker_command *dyldlc;
    struct symtab_command *symlc;
    struct dysymtab_command *dysymlc;
    char *str;

    seg = add_segment(mo, "__PAGEZERO");
    seg->vmsize = (uint64_t)1 << 32;

    seg = add_segment(mo, "__TEXT");
    seg->vmaddr = (uint64_t)1 << 32;
    seg->maxprot = 7;  // rwx
    seg->initprot = 5; // r-x

    seg = add_segment(mo, "__DATA");
    seg->vmaddr = -1;
    seg->maxprot = 7;  // rwx
    seg->initprot = 3; // rw-

    seg = add_segment(mo, "__LINKEDIT");
    seg->vmaddr = -1;
    seg->maxprot = 7;  // rwx
    seg->initprot = 1; // r--

    mo->ep = add_lc(mo, LC_MAIN, sizeof(*mo->ep));
    mo->ep->entryoff = 4096;

    i = (sizeof(*dyldlc) + strlen("/usr/lib/dyld") + 1 + 7) &-8;
    dyldlc = add_lc(mo, LC_LOAD_DYLINKER, i);
    dyldlc->name = sizeof(*dyldlc);
    str = (char*)dyldlc + dyldlc->name;
    strcpy(str, "/usr/lib/dyld");

    mo->dyldinfo = add_lc(mo, LC_DYLD_INFO_ONLY, sizeof(*mo->dyldinfo));
    symlc = add_lc(mo, LC_SYMTAB, sizeof(*symlc));
    dysymlc = add_lc(mo, LC_DYSYMTAB, sizeof(*dysymlc));

    for(i = 0; i < s1->nb_loaded_dlls; i++) {
        DLLReference *dllref = s1->loaded_dlls[i];
        if (dllref->level == 0)
          add_dylib(mo, dllref->name);
    }

    /* dyld requires a writable segment with classic Mach-O, but it ignores
       zero-sized segments for this, so force to have some data.  */
    section_ptr_add(data_section, 1);
    memset (mo->sk_to_sect, 0, sizeof(mo->sk_to_sect));
    for (i = s1->nb_sections; i-- > 1;) {
        int type, flags;
        s = s1->sections[i];
        type = s->sh_type;
        flags = s->sh_flags;
        sk = sk_unknown;
        if (flags & SHF_ALLOC) {
            switch (type) {
            default:           sk = sk_unknown; break;
            case SHT_INIT_ARRAY: sk = sk_init; break;
            case SHT_FINI_ARRAY: sk = sk_fini; break;
            case SHT_NOBITS:   sk = sk_bss; break;
            case SHT_SYMTAB:   sk = sk_discard; break;
            case SHT_STRTAB:
		if (s == stabstr_section)
		  sk = sk_stab_str;
		else
		  sk = sk_discard;
		break;
            case SHT_RELX:     sk = sk_discard; break;
            case SHT_LINKEDIT: sk = sk_linkedit; break;
            case SHT_PROGBITS:
                if (s == mo->stubs)
                  sk = sk_stubs;
                else if (s == mo->stub_helper)
                  sk = sk_stub_helper;
                else if (s == s1->got)
                  sk = sk_nl_ptr;
                else if (s == mo->la_symbol_ptr)
                  sk = sk_la_ptr;
                else if (s == stab_section)
                  sk = sk_stab;
                else if (s == dwarf_info_section)
                  sk = sk_debug_info;
                else if (s == dwarf_abbrev_section)
                  sk = sk_debug_abbrev;
                else if (s == dwarf_line_section)
                  sk = sk_debug_line;
                else if (s == dwarf_aranges_section)
                  sk = sk_debug_aranges;
                else if (s == dwarf_str_section)
                  sk = sk_debug_str;
                else if (s == dwarf_line_str_section)
                  sk = sk_debug_line_str;
                else if (flags & SHF_EXECINSTR)
                  sk = sk_text;
                else if (flags & SHF_WRITE)
                  sk = sk_rw_data;
                else
                  sk = sk_ro_data;
                break;
            }
        } else
          sk = sk_discard;
        s->prev = mo->sk_to_sect[sk].s;
        mo->sk_to_sect[sk].s = s;
    }
    fileofs = 4096;  /* leave space for mach-o headers */
    curaddr = get_segment(mo, 1)->vmaddr;
    curaddr += 4096;
    seg = NULL;
    numsec = 0;
    mo->elfsectomacho = tcc_mallocz(sizeof(*mo->elfsectomacho) * s1->nb_sections);
    for (sk = sk_unknown; sk < sk_last; sk++) {
        struct section_64 *sec = NULL;
#define SEG_PAGE_SIZE 16384
	if (sk == sk_linkedit)
	    do_bind_rebase(s1, mo);
        if (seg) {
            seg->vmsize = (curaddr - seg->vmaddr + SEG_PAGE_SIZE - 1) & -SEG_PAGE_SIZE;
            seg->filesize = (fileofs - seg->fileoff + SEG_PAGE_SIZE - 1) & -SEG_PAGE_SIZE;
            curaddr = seg->vmaddr + seg->vmsize;
            fileofs = seg->fileoff + seg->filesize;
        }
        if (skinfo[sk].seg && mo->sk_to_sect[sk].s) {
            uint64_t al = 0;
            int si;
            seg = get_segment(mo, skinfo[sk].seg);
            if (skinfo[sk].name) {
                si = add_section(mo, &seg, skinfo[sk].name);
                numsec++;
                mo->lc[mo->seg2lc[skinfo[sk].seg]] = (struct load_command*)seg;
                mo->sk_to_sect[sk].machosect = si;
                sec = get_section(seg, si);
                sec->flags = skinfo[sk].flags;
		if (sk == sk_stubs)
#ifdef TCC_TARGET_X86_64
	    	    sec->reserved2 = 6;
#elif defined TCC_TARGET_ARM64
	    	    sec->reserved2 = 12;
#endif
		if (sk == sk_nl_ptr)
	    	    sec->reserved1 = mo->nr_plt;
		if (sk == sk_la_ptr)
	    	    sec->reserved1 = mo->nr_plt + mo->n_got;
            }
            if (seg->vmaddr == -1) {
                curaddr = (curaddr + 4095) & -4096;
                seg->vmaddr = curaddr;
                fileofs = (fileofs + 4095) & -4096;
                seg->fileoff = fileofs;
            }

            for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
                int a = exact_log2p1(s->sh_addralign);
                if (a && al < (a - 1))
                  al = a - 1;
                s->sh_size = s->data_offset;
            }
            if (sec)
              sec->align = al;
            al = 1ULL << al;
            if (al > 4096)
              tcc_warning("alignment > 4096"), sec->align = 12, al = 4096;
            curaddr = (curaddr + al - 1) & -al;
            fileofs = (fileofs + al - 1) & -al;
            if (sec) {
                sec->addr = curaddr;
                sec->offset = fileofs;
            }
            for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
                al = s->sh_addralign;
                curaddr = (curaddr + al - 1) & -al;
                dprintf("curaddr now 0x%lx\n", (long)curaddr);
                s->sh_addr = curaddr;
                curaddr += s->sh_size;
                if (s->sh_type != SHT_NOBITS) {
                    fileofs = (fileofs + al - 1) & -al;
                    s->sh_offset = fileofs;
                    fileofs += s->sh_size;
                    dprintf("fileofs now %ld\n", (long)fileofs);
                }
                if (sec)
                  mo->elfsectomacho[s->sh_num] = numsec;
            }
            if (sec)
              sec->size = curaddr - sec->addr;
        }
        if (DEBUG_MACHO)
          for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
              int type = s->sh_type;
              int flags = s->sh_flags;
              printf("%d section %-16s %-10s %09lx %04x %02d %s,%s,%s\n",
                     sk,
                     s->name,
                     type == SHT_PROGBITS ? "progbits" :
                     type == SHT_NOBITS ? "nobits" :
                     type == SHT_SYMTAB ? "symtab" :
                     type == SHT_STRTAB ? "strtab" :
                     type == SHT_INIT_ARRAY ? "init" :
                     type == SHT_FINI_ARRAY ? "fini" :
                     type == SHT_RELX ? "rel" : "???",
                     (long)s->sh_addr,
                     (unsigned)s->data_offset,
                     s->sh_addralign,
                     flags & SHF_ALLOC ? "alloc" : "",
                     flags & SHF_WRITE ? "write" : "",
                     flags & SHF_EXECINSTR ? "exec" : ""
                    );
          }
    }
    if (seg) {
        seg->vmsize = curaddr - seg->vmaddr;
        seg->filesize = fileofs - seg->fileoff;
    }

    /* Fill symtab info */
    symlc->symoff = mo->symtab->sh_offset;
    symlc->nsyms = mo->symtab->data_offset / sizeof(struct nlist_64);
    symlc->stroff = mo->strtab->sh_offset;
    symlc->strsize = mo->strtab->data_offset;

    dysymlc->iundefsym = mo->iundef == -1 ? symlc->nsyms : mo->iundef;
    dysymlc->iextdefsym = mo->iextdef == -1 ? dysymlc->iundefsym : mo->iextdef;
    dysymlc->ilocalsym = mo->ilocal == -1 ? dysymlc->iextdefsym : mo->ilocal;
    dysymlc->nlocalsym = dysymlc->iextdefsym - dysymlc->ilocalsym;
    dysymlc->nextdefsym = dysymlc->iundefsym - dysymlc->iextdefsym;
    dysymlc->nundefsym = symlc->nsyms - dysymlc->iundefsym;
    dysymlc->indirectsymoff = mo->indirsyms->sh_offset;
    dysymlc->nindirectsyms = mo->indirsyms->data_offset / sizeof(uint32_t);

    if (mo->rebase->data_offset) {
        mo->dyldinfo->rebase_off = mo->rebase->sh_offset;
        mo->dyldinfo->rebase_size = mo->rebase->data_offset;
    }
    if (mo->binding->data_offset) {
        mo->dyldinfo->bind_off = mo->binding->sh_offset;
        mo->dyldinfo->bind_size = mo->binding->data_offset;
    }
    if (mo->weak_binding->data_offset) {
        mo->dyldinfo->weak_bind_off = mo->weak_binding->sh_offset;
        mo->dyldinfo->weak_bind_size = mo->weak_binding->data_offset;
    }
    if (mo->lazy_binding->data_offset) {
        mo->dyldinfo->lazy_bind_off = mo->lazy_binding->sh_offset;
        mo->dyldinfo->lazy_bind_size = mo->lazy_binding->data_offset;
    }
    if (mo->exports->data_offset) {
        mo->dyldinfo->export_off = mo->exports->sh_offset;
        mo->dyldinfo->export_size = mo->exports->data_offset;
    }
}

static void macho_write(TCCState *s1, struct macho *mo, FILE *fp)
{
    int i, sk;
    uint64_t fileofs = 0;
    Section *s;
    mo->mh.mh.magic = MH_MAGIC_64;
#ifdef TCC_TARGET_X86_64
    mo->mh.mh.cputype = CPU_TYPE_X86_64;
    mo->mh.mh.cpusubtype = CPU_SUBTYPE_LIB64 | CPU_SUBTYPE_X86_ALL;
#elif defined TCC_TARGET_ARM64
    mo->mh.mh.cputype = CPU_TYPE_ARM64;
    mo->mh.mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
#endif
    mo->mh.mh.filetype = MH_EXECUTE;
    mo->mh.mh.flags = MH_DYLDLINK | MH_PIE;
    mo->mh.mh.ncmds = mo->nlc;
    mo->mh.mh.sizeofcmds = 0;
    for (i = 0; i < mo->nlc; i++)
      mo->mh.mh.sizeofcmds += mo->lc[i]->cmdsize;

    fwrite(&mo->mh, 1, sizeof(mo->mh), fp);
    fileofs += sizeof(mo->mh);
    for (i = 0; i < mo->nlc; i++) {
        fwrite(mo->lc[i], 1, mo->lc[i]->cmdsize, fp);
        fileofs += mo->lc[i]->cmdsize;
    }

    for (sk = sk_unknown; sk < sk_last; sk++) {
        //struct segment_command_64 *seg;
        if (!skinfo[sk].seg || !mo->sk_to_sect[sk].s)
          continue;
        /*seg =*/ get_segment(mo, skinfo[sk].seg);
        for (s = mo->sk_to_sect[sk].s; s; s = s->prev) {
            if (s->sh_type != SHT_NOBITS) {
                while (fileofs < s->sh_offset)
                  fputc(0, fp), fileofs++;
                if (s->sh_size) {
                    fwrite(s->data, 1, s->sh_size, fp);
                    fileofs += s->sh_size;
                }
            }
        }
    }
}

ST_FUNC int macho_output_file(TCCState *s1, const char *filename)
{
    int fd, mode, file_type;
    FILE *fp;
    int i, ret = -1;
    struct macho mo;

    (void)memset(&mo, 0, sizeof(mo));

    file_type = s1->output_type;
    if (file_type == TCC_OUTPUT_OBJ)
        mode = 0666;
    else
        mode = 0777;
    unlink(filename);
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, mode);
    if (fd < 0 || (fp = fdopen(fd, "wb")) == NULL) {
        tcc_error_noabort("could not write '%s: %s'", filename, strerror(errno));
        return -1;
    }
    if (s1->verbose)
        printf("<- %s\n", filename);

    tcc_add_runtime(s1);
    tcc_macho_add_destructor(s1);
    resolve_common_syms(s1);
    create_symtab(s1, &mo);
    check_relocs(s1, &mo);
    ret = check_symbols(s1, &mo);
    if (!ret) {
        collect_sections(s1, &mo);
        relocate_syms(s1, s1->symtab, 0);
        mo.ep->entryoff = get_sym_addr(s1, "main", 1, 1)
                            - get_segment(&mo, 1)->vmaddr;
        if (s1->nb_errors)
          goto do_ret;
        relocate_sections(s1);
        convert_symbols(s1, &mo);
        macho_write(s1, &mo, fp);
    }

 do_ret:
    for (i = 0; i < mo.nlc; i++)
      tcc_free(mo.lc[i]);
    tcc_free(mo.lc);
    tcc_free(mo.elfsectomacho);
    tcc_free(mo.e2msym);

    fclose(fp);
    return ret;
}

static uint32_t macho_swap32(uint32_t x)
{
  return (x >> 24) | (x << 24) | ((x >> 8) & 0xff00) | ((x & 0xff00) << 8);
}
#define SWAP(x) (swap ? macho_swap32(x) : (x))
#define tbd_parse_movepast(s) \
    (pos = (pos = strstr(pos, s)) ? pos + strlen(s) : NULL)
#define tbd_parse_movetoany(cs) (pos = strpbrk(pos, cs))
#define tbd_parse_skipws while (*pos && (*pos==' '||*pos=='\n')) ++pos
#define tbd_parse_tramplequote if(*pos=='\''||*pos=='"') tbd_parse_trample
#define tbd_parse_tramplespace if(*pos==' ') tbd_parse_trample
#define tbd_parse_trample *pos++=0

#ifdef TCC_IS_NATIVE
/* Looks for the active developer SDK set by xcode-select (or the default
   one set during installation.) */
ST_FUNC void tcc_add_macos_sdkpath(TCCState* s)
{
    char *sdkroot = NULL, *pos = NULL;
    void* xcs = dlopen("libxcselect.dylib", RTLD_GLOBAL | RTLD_LAZY);
    CString path;
    int (*f)(unsigned int, char**) = dlsym(xcs, "xcselect_host_sdk_path");
    cstr_new(&path);
    if (f) f(1, &sdkroot);
    if (sdkroot)
        pos = strstr(sdkroot,"SDKs/MacOSX");
    if (pos)
        cstr_printf(&path, "%.*s.sdk/usr/lib", (int)(pos - sdkroot + 11), sdkroot);
    /* must use free from libc directly */
#pragma push_macro("free")
#undef free
    free(sdkroot);
#pragma pop_macro("free")
    if (path.size)
        tcc_add_library_path(s, (char*)path.data);
    else
        tcc_add_library_path(s,
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib"
            ":" "/Applications/Xcode.app/Developer/SDKs/MacOSX.sdk/usr/lib"
            );
    cstr_free(&path);
}

ST_FUNC const char* macho_tbd_soname(const char* filename) {
    char *soname, *data, *pos;
    const char *ret = filename;

    int fd = open(filename,O_RDONLY);
    if (fd<0) return ret;
    pos = data = tcc_load_text(fd);
    if (!tbd_parse_movepast("install-name: ")) goto the_end;
    tbd_parse_skipws;
    tbd_parse_tramplequote;
    soname = pos;
    if (!tbd_parse_movetoany("\n \"'")) goto the_end;
    tbd_parse_trample;
    ret = tcc_strdup(soname);
the_end:
    tcc_free(data);
    return ret;
}
#endif /* TCC_IS_NATIVE */

ST_FUNC int macho_load_tbd(TCCState* s1, int fd, const char* filename, int lev)
{
    char *soname, *data, *pos;
    int ret = -1;

    pos = data = tcc_load_text(fd);
    if (!tbd_parse_movepast("install-name: ")) goto the_end;
    tbd_parse_skipws;
    tbd_parse_tramplequote;
    soname = pos;
    if (!tbd_parse_movetoany("\n \"'")) goto the_end;
    tbd_parse_trample;
    ret = 0;
    if (tcc_add_dllref(s1, soname, lev)->found)
        goto the_end;
    while(pos) {
        char* sym = NULL;
        int cont = 1;
        if (!tbd_parse_movepast("symbols: ")) break;
        if (!tbd_parse_movepast("[")) break;
        while (cont) {
            tbd_parse_skipws;
            tbd_parse_tramplequote;
            sym = pos;
            if (!tbd_parse_movetoany(",] \"'")) break;
            tbd_parse_tramplequote;
            tbd_parse_tramplespace;
            tbd_parse_skipws;
            if (*pos==0||*pos==']') cont=0;
            tbd_parse_trample;
            set_elf_sym(s1->dynsymtab_section, 0, 0,
                ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE), 0, SHN_UNDEF, sym);
        }
    }

the_end:
    tcc_free(data);
    return ret;
}

ST_FUNC int macho_load_dll(TCCState * s1, int fd, const char* filename, int lev)
{
    unsigned char buf[sizeof(struct mach_header_64)];
    void *buf2;
    uint32_t machofs = 0;
    struct fat_header fh;
    struct mach_header mh;
    struct load_command *lc;
    int i, swap = 0;
    const char *soname = filename;
    struct nlist_64 *symtab = 0;
    uint32_t nsyms = 0;
    char *strtab = 0;
    uint32_t strsize = 0;
    uint32_t iextdef = 0;
    uint32_t nextdef = 0;

  again:
    if (full_read(fd, buf, sizeof(buf)) != sizeof(buf))
      return -1;
    memcpy(&fh, buf, sizeof(fh));
    if (fh.magic == FAT_MAGIC || fh.magic == FAT_CIGAM) {
        struct fat_arch *fa = load_data(fd, sizeof(fh),
                                        fh.nfat_arch * sizeof(*fa));
        swap = fh.magic == FAT_CIGAM;
        for (i = 0; i < SWAP(fh.nfat_arch); i++)
#ifdef TCC_TARGET_X86_64
          if (SWAP(fa[i].cputype) == CPU_TYPE_X86_64
              && SWAP(fa[i].cpusubtype) == CPU_SUBTYPE_X86_ALL)
#elif defined TCC_TARGET_ARM64
          if (SWAP(fa[i].cputype) == CPU_TYPE_ARM64
              && SWAP(fa[i].cpusubtype) == CPU_SUBTYPE_ARM64_ALL)
#endif
            break;
        if (i == SWAP(fh.nfat_arch)) {
            tcc_free(fa);
            return -1;
        }
        machofs = SWAP(fa[i].offset);
        tcc_free(fa);
        lseek(fd, machofs, SEEK_SET);
        goto again;
    } else if (fh.magic == FAT_MAGIC_64 || fh.magic == FAT_CIGAM_64) {
        tcc_warning("%s: Mach-O fat 64bit files of type 0x%x not handled",
                    filename, fh.magic);
        return -1;
    }

    memcpy(&mh, buf, sizeof(mh));
    if (mh.magic != MH_MAGIC_64)
      return -1;
    dprintf("found Mach-O at %d\n", machofs);
    buf2 = load_data(fd, machofs + sizeof(struct mach_header_64), mh.sizeofcmds);
    for (i = 0, lc = buf2; i < mh.ncmds; i++) {
        dprintf("lc %2d: 0x%08x\n", i, lc->cmd);
        switch (lc->cmd) {
        case LC_SYMTAB:
        {
            struct symtab_command *sc = (struct symtab_command*)lc;
            nsyms = sc->nsyms;
            symtab = load_data(fd, machofs + sc->symoff, nsyms * sizeof(*symtab));
            strsize = sc->strsize;
            strtab = load_data(fd, machofs + sc->stroff, strsize);
            break;
        }
        case LC_ID_DYLIB:
        {
            struct dylib_command *dc = (struct dylib_command*)lc;
            soname = (char*)lc + dc->name;
            dprintf(" ID_DYLIB %d 0x%x 0x%x %s\n",
                    dc->timestamp, dc->current_version,
                    dc->compatibility_version, soname);
            break;
        }
        case LC_REEXPORT_DYLIB:
        {
            struct dylib_command *dc = (struct dylib_command*)lc;
            char *name = (char*)lc + dc->name;
            int subfd = open(name, O_RDONLY | O_BINARY);
            dprintf(" REEXPORT %s\n", name);
            if (subfd < 0)
              tcc_warning("can't open %s (reexported from %s)", name, filename);
            else {
                /* Hopefully the REEXPORTs never form a cycle, we don't check
                   for that!  */
                macho_load_dll(s1, subfd, name, lev + 1);
                close(subfd);
            }
            break;
        }
        case LC_DYSYMTAB:
        {
            struct dysymtab_command *dc = (struct dysymtab_command*)lc;
            iextdef = dc->iextdefsym;
            nextdef = dc->nextdefsym;
            break;
        }
        }
        lc = (struct load_command*) ((char*)lc + lc->cmdsize);
    }

    if (tcc_add_dllref(s1, soname, lev)->found)
        goto the_end;

    if (!nsyms || !nextdef)
      tcc_warning("%s doesn't export any symbols?", filename);

    //dprintf("symbols (all):\n");
    dprintf("symbols (exported):\n");
    dprintf("    n: typ sec   desc              value name\n");
    //for (i = 0; i < nsyms; i++) {
    for (i = iextdef; i < iextdef + nextdef; i++) {
        struct nlist_64 *sym = symtab + i;
        dprintf("%5d: %3d %3d 0x%04x 0x%016lx %s\n",
                i, sym->n_type, sym->n_sect, sym->n_desc, (long)sym->n_value,
                strtab + sym->n_strx);
        set_elf_sym(s1->dynsymtab_section, 0, 0,
                    ELFW(ST_INFO)(STB_GLOBAL, STT_NOTYPE),
                    0, SHN_UNDEF, strtab + sym->n_strx);
    }

  the_end:
    tcc_free(strtab);
    tcc_free(symtab);
    tcc_free(buf2);
    return 0;
}
