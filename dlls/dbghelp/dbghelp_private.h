/*
 * File dbghelp_private.h - dbghelp internal definitions
 *
 * Copyright (C) 1995, Alexandre Julliard
 * Copyright (C) 1996, Eric Youngdale.
 * Copyright (C) 1999-2000, Ulrich Weigand.
 * Copyright (C) 2004-2007, Eric Pouech.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winver.h"
#include "dbghelp.h"
#include "objbase.h"
#include "oaidl.h"
#include "winnls.h"
#include "wine/list.h"
#include "wine/rbtree.h"

#include "cvconst.h"

/* #define USE_STATS */

struct pool /* poor's man */
{
    struct list arena_list;
    struct list arena_full;
    size_t      arena_size;
};

void     pool_init(struct pool* a, size_t arena_size) DECLSPEC_HIDDEN;
void     pool_destroy(struct pool* a) DECLSPEC_HIDDEN;
void*    pool_alloc(struct pool* a, size_t len) DECLSPEC_HIDDEN;
char*    pool_strdup(struct pool* a, const char* str) DECLSPEC_HIDDEN;

struct vector
{
    void**      buckets;
    unsigned    elt_size;
    unsigned    shift;
    unsigned    num_elts;
    unsigned    num_buckets;
    unsigned    buckets_allocated;
};

void     vector_init(struct vector* v, unsigned elt_sz, unsigned bucket_sz) DECLSPEC_HIDDEN;
unsigned vector_length(const struct vector* v) DECLSPEC_HIDDEN;
void*    vector_at(const struct vector* v, unsigned pos) DECLSPEC_HIDDEN;
void*    vector_add(struct vector* v, struct pool* pool) DECLSPEC_HIDDEN;

struct sparse_array
{
    struct vector               key2index;
    struct vector               elements;
};

void     sparse_array_init(struct sparse_array* sa, unsigned elt_sz, unsigned bucket_sz) DECLSPEC_HIDDEN;
void*    sparse_array_find(const struct sparse_array* sa, ULONG_PTR idx) DECLSPEC_HIDDEN;
void*    sparse_array_add(struct sparse_array* sa, ULONG_PTR key, struct pool* pool) DECLSPEC_HIDDEN;
unsigned sparse_array_length(const struct sparse_array* sa) DECLSPEC_HIDDEN;

struct hash_table_elt
{
    const char*                 name;
    struct hash_table_elt*      next;
};

struct hash_table_bucket
{
    struct hash_table_elt*      first;
    struct hash_table_elt*      last;
};

struct hash_table
{
    unsigned                    num_elts;
    unsigned                    num_buckets;
    struct hash_table_bucket*   buckets;
    struct pool*                pool;
};

void     hash_table_init(struct pool* pool, struct hash_table* ht,
                         unsigned num_buckets) DECLSPEC_HIDDEN;
void     hash_table_destroy(struct hash_table* ht) DECLSPEC_HIDDEN;
void     hash_table_add(struct hash_table* ht, struct hash_table_elt* elt) DECLSPEC_HIDDEN;

struct hash_table_iter
{
    const struct hash_table*    ht;
    struct hash_table_elt*      element;
    int                         index;
    int                         last;
};

void     hash_table_iter_init(const struct hash_table* ht,
                              struct hash_table_iter* hti, const char* name) DECLSPEC_HIDDEN;
void*    hash_table_iter_up(struct hash_table_iter* hti) DECLSPEC_HIDDEN;


extern unsigned dbghelp_options DECLSPEC_HIDDEN;
extern BOOL     dbghelp_opt_native DECLSPEC_HIDDEN;
extern SYSTEM_INFO sysinfo DECLSPEC_HIDDEN;

enum location_kind {loc_error,          /* reg is the error code */
                    loc_unavailable,    /* location is not available */
                    loc_absolute,       /* offset is the location */
                    loc_register,       /* reg is the location */
                    loc_regrel,         /* [reg+offset] is the location */
                    loc_tlsrel,         /* offset is the address of the TLS index */
                    loc_user,           /* value is debug information dependent,
                                           reg & offset can be used ad libidem */
};

enum location_error {loc_err_internal = -1,     /* internal while computing */
                     loc_err_too_complex = -2,  /* couldn't compute location (even at runtime) */
                     loc_err_out_of_scope = -3, /* variable isn't available at current address */
                     loc_err_cant_read = -4,    /* couldn't read memory at given address */
                     loc_err_no_location = -5,  /* likely optimized away (by compiler) */
};

struct location
{
    unsigned            kind : 8,
                        reg;
    ULONG_PTR           offset;
};

struct symt
{
    enum SymTagEnum             tag;
};

struct symt_ht
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;        /* if global symbol or type */
};

static inline BOOL symt_check_tag(const struct symt* s, enum SymTagEnum tag)
{
    return s && s->tag == tag;
}

/* lexical tree */
struct symt_block
{
    struct symt                 symt;
    ULONG_PTR                   address;
    ULONG_PTR                   size;
    struct symt*                container;      /* block, or func */
    struct vector               vchildren;      /* sub-blocks & local variables */
};

struct symt_module /* in fact any of .exe, .dll... */
{
    struct symt                 symt;           /* module */
    struct vector               vchildren;      /* compilation units */
    struct module*              module;
};

struct symt_compiland
{
    struct symt                 symt;
    struct symt_module*         container;      /* symt_module */
    ULONG_PTR                   address;
    unsigned                    source;
    struct vector               vchildren;      /* global variables & functions */
    void*                       user;           /* when debug info provider needs to store information */
};

struct symt_data
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;       /* if global symbol */
    enum DataKind               kind;
    struct symt*                container;
    struct symt*                type;
    union                                       /* depends on kind */
    {
        /* DataIs{Global, FileStatic, StaticLocal}:
         *      with loc.kind
         *              loc_absolute    loc.offset is address
         *              loc_tlsrel      loc.offset is TLS index address
         * DataIs{Local,Param}:
         *      with loc.kind
         *              loc_absolute    not supported
         *              loc_register    location is in register loc.reg
         *              loc_regrel      location is at address loc.reg + loc.offset
         *              >= loc_user     ask debug info provider for resolution
         */
        struct location         var;
        /* DataIs{Member} (all values are in bits, not bytes) */
        struct
        {
            LONG_PTR                    offset;
            ULONG_PTR                   bit_length;
            ULONG_PTR                   bit_offset;
        } member;
        /* DataIsConstant */
        VARIANT                 value;
    } u;
};

/* We must take into account that most debug formats (dwarf and pdb) report for
 * code (esp. inlined functions) inside functions the following way:
 * - block
 *   + is represented by a contiguous area of memory,
 *     or at least have lo/hi addresses to encompass it's contents
 *   + but most importantly, block A's lo/hi range is always embedded within
 *     its parent (block or function)
 * - inline site:
 *   + is most of the times represented by a set of ranges (instead of a
 *     contiguous block)
 *   + native dbghelp only exports the start address, not its size
 *   + the set of ranges isn't always embedded in enclosing block (if any)
 *   + the set of ranges is always embedded in top function
 * - (top) function
 *   + is described as a contiguous block of memory
 *
 * On top of the items above (taken as assumptions), we also assume that:
 * - a range in inline site A, is disjoint from all the other ranges in
 *   inline site A
 * - a range in inline site A, is either disjoint or embedded into any of
 *   the ranges of inline sites parent of A
 *
 * Therefore, we also store all inline sites inside a function:
 * - available as a linked list to simplify the walk among them
 * - this linked list shall preserve the weak order of the lexical-parent
 *   relationship (eg for any inline site A, which has inline site B
 *   as lexical parent, then A is present before B in the linked list)
 * - hence (from the assumptions above), when looking up which inline site
 *   contains a given address, the first range containing that address found
 *   while walking the list of inline sites is the right one.
 */

struct symt_function
{
    struct symt                 symt;           /* SymTagFunction (or SymTagInlineSite when embedded in symt_inlinesite) */
    struct hash_table_elt       hash_elt;       /* if global symbol */
    ULONG_PTR                   address;
    struct symt*                container;      /* compiland */
    struct symt*                type;           /* points to function_signature */
    ULONG_PTR                   size;
    struct vector               vlines;
    struct vector               vchildren;      /* locals, params, blocks, start/end, labels, inline sites */
    struct symt_inlinesite*     next_inlinesite;/* linked list of inline sites in this function */
};

/* FIXME: this could be optimized later on by using relative offsets and smaller integral sizes */
struct addr_range
{
    DWORD64                     low;            /* absolute address of first byte of the range */
    DWORD64                     high;           /* absolute address of first byte after the range */
};

/* tests whether ar2 is inside ar1 */
static inline BOOL addr_range_inside(const struct addr_range* ar1, const struct addr_range* ar2)
{
    return ar1->low <= ar2->low && ar2->high <= ar1->high;
}

/* tests whether ar1 and ar2 are disjoint */
static inline BOOL addr_range_disjoint(const struct addr_range* ar1, const struct addr_range* ar2)
{
    return ar1->high <= ar2->low || ar2->high <= ar1->low;
}

/* a symt_inlinesite* can be casted to a symt_function* to access all function bits */
struct symt_inlinesite
{
    struct symt_function        func;
    struct vector               vranges;        /* of addr_range: where the inline site is actually defined */
};

struct symt_hierarchy_point
{
    struct symt                 symt;           /* either SymTagFunctionDebugStart, SymTagFunctionDebugEnd, SymTagLabel */
    struct hash_table_elt       hash_elt;       /* if label (and in compiland's hash table if global) */
    struct symt*                parent;         /* symt_function or symt_compiland */
    struct location             loc;
};

struct symt_public
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    struct symt*                container;      /* compiland */
    BOOL is_function;
    ULONG_PTR                   address;
    ULONG_PTR                   size;
};

struct symt_thunk
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    struct symt*                container;      /* compiland */
    ULONG_PTR                   address;
    ULONG_PTR                   size;
    THUNK_ORDINAL               ordinal;        /* FIXME: doesn't seem to be accessible */
};

struct symt_custom
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    DWORD64                     address;
    DWORD                       size;
};

/* class tree */
struct symt_array
{
    struct symt                 symt;
    int		                start;
    DWORD                       count;
    struct symt*                base_type;
    struct symt*                index_type;
};

struct symt_basic
{
    struct symt                 symt;
    enum BasicType              bt;
    ULONG_PTR                   size;
};

struct symt_enum
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    struct symt*                base_type;
    struct vector               vchildren;
};

struct symt_function_signature
{
    struct symt                 symt;
    struct symt*                rettype;
    struct vector               vchildren;
    enum CV_call_e              call_conv;
};

struct symt_function_arg_type
{
    struct symt                 symt;
    struct symt*                arg_type;
};

struct symt_pointer
{
    struct symt                 symt;
    struct symt*                pointsto;
    ULONG_PTR                   size;
};

struct symt_typedef
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    struct symt*                type;
};

struct symt_udt
{
    struct symt                 symt;
    struct hash_table_elt       hash_elt;
    enum UdtKind                kind;
    int		                size;
    struct vector               vchildren;
};

enum module_type
{
    DMT_UNKNOWN,        /* for lookup, not actually used for a module */
    DMT_ELF,            /* a real ELF shared module */
    DMT_PE,             /* a native or builtin PE module */
    DMT_MACHO,          /* a real Mach-O shared module */
    DMT_PDB,            /* .PDB file */
    DMT_DBG,            /* .DBG file */
};

struct process;
struct module;

/* a module can be made of several debug information formats, so we have to
 * support them all
 */
enum format_info
{
    DFI_ELF,
    DFI_PE,
    DFI_MACHO,
    DFI_DWARF,
    DFI_PDB,
    DFI_LAST
};

struct module_format
{
    struct module*              module;
    void                        (*remove)(struct process* pcs, struct module_format* modfmt);
    void                        (*loc_compute)(struct process* pcs,
                                               const struct module_format* modfmt,
                                               const struct symt_function* func,
                                               struct location* loc);
    union
    {
        struct elf_module_info*         elf_info;
        struct dwarf2_module_info_s*    dwarf2_info;
        struct pe_module_info*          pe_info;
        struct macho_module_info*	macho_info;
        struct pdb_module_info*         pdb_info;
    } u;
};

struct cpu;

struct module
{
    struct process*             process;
    IMAGEHLP_MODULEW64          module;
    WCHAR                       modulename[64]; /* used for enumeration */
    struct module*              next;
    enum module_type		type : 16;
    unsigned short              is_virtual : 1;
    struct cpu*                 cpu;
    DWORD64                     reloc_delta;
    WCHAR*                      real_path;

    /* specific information for debug types */
    struct module_format*       format_info[DFI_LAST];

    /* memory allocation pool */
    struct pool                 pool;

    /* symbols & symbol tables */
    struct vector               vsymt;
    struct vector               vcustom_symt;
    int                         sortlist_valid;
    unsigned                    num_sorttab;    /* number of symbols with addresses */
    unsigned                    num_symbols;
    unsigned                    sorttab_size;
    struct symt_ht**            addr_sorttab;
    struct hash_table           ht_symbols;
    struct symt_module*         top;

    /* types */
    struct hash_table           ht_types;
    struct vector               vtypes;

    /* source files */
    unsigned                    sources_used;
    unsigned                    sources_alloc;
    char*                       sources;
    struct wine_rb_tree         sources_offsets_tree;
};

typedef BOOL (*enum_modules_cb)(const WCHAR*, ULONG_PTR addr, void* user);

struct loader_ops
{
    BOOL (*synchronize_module_list)(struct process* process);
    struct module* (*load_module)(struct process* process, const WCHAR* name, ULONG_PTR addr);
    BOOL (*load_debug_info)(struct process *process, struct module* module);
    BOOL (*enum_modules)(struct process* process, enum_modules_cb callback, void* user);
    BOOL (*fetch_file_info)(struct process* process, const WCHAR* name, ULONG_PTR load_addr, DWORD_PTR* base, DWORD* size, DWORD* checksum);
};

struct process 
{
    struct process*             next;
    HANDLE                      handle;
    const struct loader_ops*    loader;
    WCHAR*                      search_path;
    WCHAR*                      environment;

    PSYMBOL_REGISTERED_CALLBACK64       reg_cb;
    PSYMBOL_REGISTERED_CALLBACK reg_cb32;
    BOOL                        reg_is_unicode;
    DWORD64                     reg_user;

    struct module*              lmodules;
    ULONG_PTR                   dbg_hdr_addr;

    IMAGEHLP_STACK_FRAME        ctx_frame;
    DWORD64                     localscope_pc;
    struct symt*                localscope_symt;

    unsigned                    buffer_size;
    void*                       buffer;

    BOOL                        is_64bit;
};

static inline BOOL read_process_memory(const struct process *process, UINT64 addr, void *buf, size_t size)
{
    return ReadProcessMemory(process->handle, (void*)(UINT_PTR)addr, buf, size, NULL);
}

struct line_info
{
    ULONG_PTR                   is_first : 1,
                                is_last : 1,
                                is_source_file : 1,
                                line_number;
    union
    {
        ULONG_PTR                   address;     /* absolute, if is_source_file isn't set */
        unsigned                    source_file; /* if is_source_file is set */
    } u;
};

struct module_pair
{
    struct process*             pcs;
    struct module*              requested; /* in:  to module_get_debug() */
    struct module*              effective; /* out: module with debug info */
};

enum pdb_kind {PDB_JG, PDB_DS};

struct pdb_lookup
{
    const char*                 filename;
    enum pdb_kind               kind;
    unsigned int                age;
    unsigned int                timestamp;
    GUID                        guid;
};

struct cpu_stack_walk
{
    HANDLE                      hProcess;
    HANDLE                      hThread;
    BOOL                        is32;
    struct cpu *                cpu;
    union
    {
        struct
        {
            PREAD_PROCESS_MEMORY_ROUTINE        f_read_mem;
            PTRANSLATE_ADDRESS_ROUTINE          f_xlat_adr;
            PFUNCTION_TABLE_ACCESS_ROUTINE      f_tabl_acs;
            PGET_MODULE_BASE_ROUTINE            f_modl_bas;
        } s32;
        struct
        {
            PREAD_PROCESS_MEMORY_ROUTINE64      f_read_mem;
            PTRANSLATE_ADDRESS_ROUTINE64        f_xlat_adr;
            PFUNCTION_TABLE_ACCESS_ROUTINE64    f_tabl_acs;
            PGET_MODULE_BASE_ROUTINE64          f_modl_bas;
        } s64;
    } u;
};

struct dump_memory
{
    ULONG64                             base;
    ULONG                               size;
    ULONG                               rva;
};

struct dump_memory64
{
    ULONG64                             base;
    ULONG64                             size;
};

struct dump_module
{
    unsigned                            is_elf;
    ULONG64                             base;
    ULONG                               size;
    DWORD                               timestamp;
    DWORD                               checksum;
    WCHAR                               name[MAX_PATH];
};

struct dump_thread
{
    ULONG                               tid;
    ULONG                               prio_class;
    ULONG                               curr_prio;
};

struct dump_context
{
    /* process & thread information */
    struct process                     *process;
    DWORD                               pid;
    unsigned                            flags_out;
    /* thread information */
    struct dump_thread*                 threads;
    unsigned                            num_threads;
    /* module information */
    struct dump_module*                 modules;
    unsigned                            num_modules;
    unsigned                            alloc_modules;
    /* exception information */
    /* output information */
    MINIDUMP_TYPE                       type;
    HANDLE                              hFile;
    RVA                                 rva;
    struct dump_memory*                 mem;
    unsigned                            num_mem;
    unsigned                            alloc_mem;
    struct dump_memory64*               mem64;
    unsigned                            num_mem64;
    unsigned                            alloc_mem64;
    /* callback information */
    MINIDUMP_CALLBACK_INFORMATION*      cb;
};

union ctx
{
    CONTEXT ctx;
    WOW64_CONTEXT x86;
};

enum cpu_addr {cpu_addr_pc, cpu_addr_stack, cpu_addr_frame};
struct cpu
{
    DWORD       machine;
    DWORD       word_size;
    DWORD       frame_regno;

    /* address manipulation */
    BOOL        (*get_addr)(HANDLE hThread, const CONTEXT* ctx,
                            enum cpu_addr, ADDRESS64* addr);

    /* stack manipulation */
    BOOL        (*stack_walk)(struct cpu_stack_walk *csw, STACKFRAME64 *frame,
                              union ctx *ctx);

    /* module manipulation */
    void*       (*find_runtime_function)(struct module*, DWORD64 addr);

    /* dwarf dedicated information */
    unsigned    (*map_dwarf_register)(unsigned regno, const struct module* module, BOOL eh_frame);

    /* context related manipulation */
    void *      (*fetch_context_reg)(union ctx *ctx, unsigned regno, unsigned *size);
    const char* (*fetch_regname)(unsigned regno);

    /* minidump per CPU extension */
    BOOL        (*fetch_minidump_thread)(struct dump_context* dc, unsigned index, unsigned flags, const CONTEXT* ctx);
    BOOL        (*fetch_minidump_module)(struct dump_context* dc, unsigned index, unsigned flags);
};

extern struct cpu*      dbghelp_current_cpu DECLSPEC_HIDDEN;

/* PDB and Codeview */

struct msc_debug_info
{
    struct module*              module;
    int                         nsect;
    const IMAGE_SECTION_HEADER* sectp;
    int                         nomap;
    const OMAP*                 omapp;
    const BYTE*                 root;
};

/* coff.c */
extern BOOL coff_process_info(const struct msc_debug_info* msc_dbg) DECLSPEC_HIDDEN;

/* dbghelp.c */
extern struct process* process_find_by_handle(HANDLE hProcess) DECLSPEC_HIDDEN;
extern BOOL         validate_addr64(DWORD64 addr) DECLSPEC_HIDDEN;
extern BOOL         pcs_callback(const struct process* pcs, ULONG action, void* data) DECLSPEC_HIDDEN;
extern void*        fetch_buffer(struct process* pcs, unsigned size) DECLSPEC_HIDDEN;
extern const char*  wine_dbgstr_addr(const ADDRESS64* addr) DECLSPEC_HIDDEN;
extern struct cpu*  cpu_find(DWORD) DECLSPEC_HIDDEN;
extern const WCHAR *process_getenv(const struct process *process, const WCHAR *name);
extern DWORD calc_crc32(HANDLE handle) DECLSPEC_HIDDEN;

/* elf_module.c */
extern BOOL         elf_read_wine_loader_dbg_info(struct process* pcs, ULONG_PTR addr) DECLSPEC_HIDDEN;
struct elf_thunk_area;
extern int          elf_is_in_thunk_area(ULONG_PTR addr, const struct elf_thunk_area* thunks) DECLSPEC_HIDDEN;

/* macho_module.c */
extern BOOL         macho_read_wine_loader_dbg_info(struct process* pcs, ULONG_PTR addr) DECLSPEC_HIDDEN;

/* minidump.c */
void minidump_add_memory_block(struct dump_context* dc, ULONG64 base, ULONG size, ULONG rva) DECLSPEC_HIDDEN;

/* module.c */
extern const WCHAR      S_ElfW[] DECLSPEC_HIDDEN;
extern const WCHAR      S_WineLoaderW[] DECLSPEC_HIDDEN;
extern const struct loader_ops no_loader_ops DECLSPEC_HIDDEN;

extern BOOL         module_init_pair(struct module_pair* pair, HANDLE hProcess,
                                     DWORD64 addr) DECLSPEC_HIDDEN;
extern struct module*
                    module_find_by_addr(const struct process* pcs, DWORD64 addr,
                                        enum module_type type) DECLSPEC_HIDDEN;
extern struct module*
                    module_find_by_nameW(const struct process* pcs,
                                         const WCHAR* name) DECLSPEC_HIDDEN;
extern struct module*
                    module_find_by_nameA(const struct process* pcs,
                                         const char* name) DECLSPEC_HIDDEN;
extern struct module*
                    module_is_already_loaded(const struct process* pcs,
                                             const WCHAR* imgname) DECLSPEC_HIDDEN;
extern BOOL         module_get_debug(struct module_pair*) DECLSPEC_HIDDEN;
extern struct module*
                    module_new(struct process* pcs, const WCHAR* name,
                               enum module_type type, BOOL virtual,
                               DWORD64 addr, DWORD64 size,
                               ULONG_PTR stamp, ULONG_PTR checksum, WORD machine) DECLSPEC_HIDDEN;
extern struct module*
                    module_get_containee(const struct process* pcs,
                                         const struct module* inner) DECLSPEC_HIDDEN;
extern void         module_reset_debug_info(struct module* module) DECLSPEC_HIDDEN;
extern BOOL         module_remove(struct process* pcs,
                                  struct module* module) DECLSPEC_HIDDEN;
extern void         module_set_module(struct module* module, const WCHAR* name) DECLSPEC_HIDDEN;
extern WCHAR*       get_wine_loader_name(struct process *pcs) DECLSPEC_HIDDEN;

/* msc.c */
extern BOOL         pe_load_debug_directory(const struct process* pcs,
                                            struct module* module, 
                                            const BYTE* mapping,
                                            const IMAGE_SECTION_HEADER* sectp, DWORD nsect,
                                            const IMAGE_DEBUG_DIRECTORY* dbg, int nDbg) DECLSPEC_HIDDEN;
extern BOOL         pdb_fetch_file_info(const struct pdb_lookup* pdb_lookup, unsigned* matched) DECLSPEC_HIDDEN;
struct pdb_cmd_pair {
    const char*         name;
    DWORD*              pvalue;
};
extern BOOL pdb_virtual_unwind(struct cpu_stack_walk *csw, DWORD_PTR ip,
    union ctx *context, struct pdb_cmd_pair *cpair) DECLSPEC_HIDDEN;

/* path.c */
extern BOOL         path_find_symbol_file(const struct process* pcs, const struct module* module,
                                          PCSTR full_path, enum module_type type, const GUID* guid, DWORD dw1, DWORD dw2,
                                          WCHAR *buffer, BOOL* is_unmatched) DECLSPEC_HIDDEN;
extern WCHAR *get_dos_file_name(const WCHAR *filename) DECLSPEC_HIDDEN;
extern BOOL search_dll_path(const struct process* process, const WCHAR *name,
                            BOOL (*match)(void*, HANDLE, const WCHAR*), void *param) DECLSPEC_HIDDEN;
extern BOOL search_unix_path(const WCHAR *name, const WCHAR *path, BOOL (*match)(void*, HANDLE, const WCHAR*), void *param) DECLSPEC_HIDDEN;
extern const WCHAR* file_name(const WCHAR* str) DECLSPEC_HIDDEN;
extern const char* file_nameA(const char* str) DECLSPEC_HIDDEN;

/* pe_module.c */
extern BOOL         pe_load_nt_header(HANDLE hProc, DWORD64 base, IMAGE_NT_HEADERS* nth) DECLSPEC_HIDDEN;
extern struct module*
                    pe_load_native_module(struct process* pcs, const WCHAR* name,
                                          HANDLE hFile, DWORD64 base, DWORD size) DECLSPEC_HIDDEN;
extern struct module*
                    pe_load_builtin_module(struct process* pcs, const WCHAR* name,
                                           DWORD64 base, DWORD64 size) DECLSPEC_HIDDEN;
extern BOOL         pe_load_debug_info(const struct process* pcs,
                                       struct module* module) DECLSPEC_HIDDEN;
extern const char*  pe_map_directory(struct module* module, int dirno, DWORD* size) DECLSPEC_HIDDEN;

/* source.c */
extern unsigned     source_new(struct module* module, const char* basedir, const char* source) DECLSPEC_HIDDEN;
extern const char*  source_get(const struct module* module, unsigned idx) DECLSPEC_HIDDEN;
extern int          source_rb_compare(const void *key, const struct wine_rb_entry *entry) DECLSPEC_HIDDEN;

/* stabs.c */
typedef void (*stabs_def_cb)(struct module* module, ULONG_PTR load_offset,
                                const char* name, ULONG_PTR offset,
                                BOOL is_public, BOOL is_global, unsigned char other,
                                struct symt_compiland* compiland, void* user);
extern BOOL         stabs_parse(struct module* module, ULONG_PTR load_offset,
                                const char* stabs, size_t nstab, size_t stabsize,
                                const char* strs, int strtablen,
                                stabs_def_cb callback, void* user) DECLSPEC_HIDDEN;

/* dwarf.c */
struct image_file_map;
extern BOOL         dwarf2_parse(struct module* module, ULONG_PTR load_offset,
                                 const struct elf_thunk_area* thunks,
                                 struct image_file_map* fmap) DECLSPEC_HIDDEN;
extern BOOL dwarf2_virtual_unwind(struct cpu_stack_walk *csw, DWORD_PTR ip,
    union ctx *ctx, DWORD64 *cfa) DECLSPEC_HIDDEN;

/* stack.c */
extern BOOL         sw_read_mem(struct cpu_stack_walk* csw, DWORD64 addr, void* ptr, DWORD sz) DECLSPEC_HIDDEN;
extern DWORD64      sw_xlat_addr(struct cpu_stack_walk* csw, ADDRESS64* addr) DECLSPEC_HIDDEN;
extern void*        sw_table_access(struct cpu_stack_walk* csw, DWORD64 addr) DECLSPEC_HIDDEN;
extern DWORD64      sw_module_base(struct cpu_stack_walk* csw, DWORD64 addr) DECLSPEC_HIDDEN;

/* symbol.c */
extern const char*  symt_get_name(const struct symt* sym) DECLSPEC_HIDDEN;
extern WCHAR*       symt_get_nameW(const struct symt* sym) DECLSPEC_HIDDEN;
extern BOOL         symt_get_address(const struct symt* type, ULONG64* addr) DECLSPEC_HIDDEN;
extern int __cdecl  symt_cmp_addr(const void* p1, const void* p2) DECLSPEC_HIDDEN;
extern void         copy_symbolW(SYMBOL_INFOW* siw, const SYMBOL_INFO* si) DECLSPEC_HIDDEN;
extern void         symbol_setname(SYMBOL_INFO* si, const char* name) DECLSPEC_HIDDEN;
extern struct symt_ht*
                    symt_find_nearest(struct module* module, DWORD_PTR addr) DECLSPEC_HIDDEN;
extern struct symt_ht*
                    symt_find_symbol_at(struct module* module, DWORD_PTR addr) DECLSPEC_HIDDEN;
extern struct symt_module*
                    symt_new_module(struct module* module) DECLSPEC_HIDDEN;
extern struct symt_compiland*
                    symt_new_compiland(struct module* module, unsigned src_idx) DECLSPEC_HIDDEN;
extern struct symt_public*
                    symt_new_public(struct module* module, 
                                    struct symt_compiland* parent, 
                                    const char* typename,
                                    BOOL is_function,
                                    ULONG_PTR address,
                                    unsigned size) DECLSPEC_HIDDEN;
extern struct symt_data*
                    symt_new_global_variable(struct module* module, 
                                             struct symt_compiland* parent,
                                             const char* name, unsigned is_static,
                                             struct location loc, ULONG_PTR size,
                                             struct symt* type) DECLSPEC_HIDDEN;
extern struct symt_function*
                    symt_new_function(struct module* module,
                                      struct symt_compiland* parent,
                                      const char* name,
                                      ULONG_PTR addr, ULONG_PTR size,
                                      struct symt* type) DECLSPEC_HIDDEN;
extern struct symt_inlinesite*
                    symt_new_inlinesite(struct module* module,
                                        struct symt_function* func,
                                        struct symt* parent,
                                        const char* name,
                                        ULONG_PTR addr,
                                        struct symt* type) DECLSPEC_HIDDEN;
extern void         symt_add_func_line(struct module* module,
                                       struct symt_function* func, 
                                       unsigned source_idx, int line_num, 
                                       ULONG_PTR offset) DECLSPEC_HIDDEN;
extern struct symt_data*
                    symt_add_func_local(struct module* module, 
                                        struct symt_function* func, 
                                        enum DataKind dt, const struct location* loc,
                                        struct symt_block* block,
                                        struct symt* type, const char* name) DECLSPEC_HIDDEN;
extern struct symt_data*
                    symt_add_func_constant(struct module* module,
                                           struct symt_function* func, struct symt_block* block,
                                           struct symt* type, const char* name, VARIANT* v) DECLSPEC_HIDDEN;
extern struct symt_block*
                    symt_open_func_block(struct module* module, 
                                         struct symt_function* func,
                                         struct symt_block* block, 
                                         unsigned pc, unsigned len) DECLSPEC_HIDDEN;
extern struct symt_block*
                    symt_close_func_block(struct module* module,
                                          const struct symt_function* func,
                                          struct symt_block* block) DECLSPEC_HIDDEN;
extern struct symt_hierarchy_point*
                    symt_add_function_point(struct module* module, 
                                            struct symt_function* func,
                                            enum SymTagEnum point, 
                                            const struct location* loc,
                                            const char* name) DECLSPEC_HIDDEN;
extern BOOL         symt_add_inlinesite_range(struct module* module,
                                              struct symt_inlinesite* inlined,
                                              ULONG_PTR low, ULONG_PTR high) DECLSPEC_HIDDEN;
extern struct symt_thunk*
                    symt_new_thunk(struct module* module, 
                                   struct symt_compiland* parent,
                                   const char* name, THUNK_ORDINAL ord,
                                   ULONG_PTR addr, ULONG_PTR size) DECLSPEC_HIDDEN;
extern struct symt_data*
                    symt_new_constant(struct module* module,
                                      struct symt_compiland* parent,
                                      const char* name, struct symt* type,
                                      const VARIANT* v) DECLSPEC_HIDDEN;
extern struct symt_hierarchy_point*
                    symt_new_label(struct module* module,
                                   struct symt_compiland* compiland,
                                   const char* name, ULONG_PTR address) DECLSPEC_HIDDEN;
extern struct symt* symt_index2ptr(struct module* module, DWORD id) DECLSPEC_HIDDEN;
extern DWORD        symt_ptr2index(struct module* module, const struct symt* sym) DECLSPEC_HIDDEN;
extern struct symt_custom*
                    symt_new_custom(struct module* module, const char* name,
                                    DWORD64 addr, DWORD size) DECLSPEC_HIDDEN;

/* type.c */
extern void         symt_init_basic(struct module* module) DECLSPEC_HIDDEN;
extern BOOL         symt_get_info(struct module* module, const struct symt* type,
                                  IMAGEHLP_SYMBOL_TYPE_INFO req, void* pInfo) DECLSPEC_HIDDEN;
extern struct symt_basic*
                    symt_get_basic(enum BasicType, unsigned size) DECLSPEC_HIDDEN;
extern struct symt_udt*
                    symt_new_udt(struct module* module, const char* typename,
                                 unsigned size, enum UdtKind kind) DECLSPEC_HIDDEN;
extern BOOL         symt_set_udt_size(struct module* module,
                                      struct symt_udt* type, unsigned size) DECLSPEC_HIDDEN;
extern BOOL         symt_add_udt_element(struct module* module, 
                                         struct symt_udt* udt_type, 
                                         const char* name,
                                         struct symt* elt_type, unsigned offset, 
                                         unsigned bit_offset, unsigned bit_size) DECLSPEC_HIDDEN;
extern struct symt_enum*
                    symt_new_enum(struct module* module, const char* typename,
                                  struct symt* basetype) DECLSPEC_HIDDEN;
extern BOOL         symt_add_enum_element(struct module* module, 
                                          struct symt_enum* enum_type, 
                                          const char* name, int value) DECLSPEC_HIDDEN;
extern struct symt_array*
                    symt_new_array(struct module* module, int min, DWORD count,
                                   struct symt* base, struct symt* index) DECLSPEC_HIDDEN;
extern struct symt_function_signature*
                    symt_new_function_signature(struct module* module, 
                                                struct symt* ret_type,
                                                enum CV_call_e call_conv) DECLSPEC_HIDDEN;
extern BOOL         symt_add_function_signature_parameter(struct module* module,
                                                          struct symt_function_signature* sig,
                                                          struct symt* param) DECLSPEC_HIDDEN;
extern struct symt_pointer*
                    symt_new_pointer(struct module* module, 
                                     struct symt* ref_type,
                                     ULONG_PTR size) DECLSPEC_HIDDEN;
extern struct symt_typedef*
                    symt_new_typedef(struct module* module, struct symt* ref, 
                                     const char* name) DECLSPEC_HIDDEN;
extern struct symt_inlinesite*
                    symt_find_lowest_inlined(struct symt_function* func, DWORD64 addr) DECLSPEC_HIDDEN;
extern struct symt*
                    symt_get_upper_inlined(struct symt_inlinesite* inlined) DECLSPEC_HIDDEN;
static inline struct symt_function*
                    symt_get_function_from_inlined(struct symt_inlinesite* inlined)
{
    while (!symt_check_tag(&inlined->func.symt, SymTagFunction))
        inlined = (struct symt_inlinesite*)symt_get_upper_inlined(inlined);
    return &inlined->func;
}
extern struct symt_inlinesite*
                    symt_find_inlined_site(struct module* module,
                                           DWORD64 addr, DWORD inline_ctx) DECLSPEC_HIDDEN;
extern DWORD        symt_get_inlinesite_depth(HANDLE hProcess, DWORD64 addr) DECLSPEC_HIDDEN;

/* Inline context encoding (different from what native does):
 * bits 31:30: 3 ignore (includes INLINE_FRAME_CONTEXT_IGNORE=0xFFFFFFFF)
 *             2 regular frame
 *             1 frame with inlined function(s).
 *             0 init   (includes INLINE_FRAME_CONTEXT_INIT=0)
 * so either stackwalkex is called with:
 * - inlinectx=IGNORE, and we use (old) StackWalk64 behavior:
 * - inlinectx=INIT, and StackWalkEx will upon return swing back&forth between:
 *      INLINE when the frame is from an inline site (inside a function)
 *      REGULAR when the frame is for a function without inline site
 * bits 29:00  depth of inline site (way too big!!)
 *             0 being the lowest inline site
 */
#define IFC_MODE_IGNORE  0xC0000000
#define IFC_MODE_REGULAR 0x80000000
#define IFC_MODE_INLINE  0x40000000
#define IFC_MODE_INIT    0x00000000
#define IFC_DEPTH_MASK   0x3FFFFFFF
#define IFC_MODE(x)      ((x) & ~IFC_DEPTH_MASK)
#define IFC_DEPTH(x)     ((x) & IFC_DEPTH_MASK)
