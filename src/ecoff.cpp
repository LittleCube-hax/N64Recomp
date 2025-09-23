#include <optional>

#include "fmt/format.h"
// #include "fmt/ostream.h"

#include "recompiler/context.h"

#define MDEBUG_INTS_SIZE 23

#define BE_TO_LE_16(x, off) ((x[off++] << 8) | x[off++])
#define BE_TO_LE_32(x, off) ((((uint8_t) (x[off++])) << 24) | (((uint8_t) (x[off++])) << 16) | (((uint8_t) (x[off++])) << 8) | ((uint8_t) (x[off++])))

#define SWAP_16(x) x = (((uint8_t) x) << 8) | ((uint8_t) (x >> 8))
#define SWAP_32(x) x = ((((uint8_t) x) << 24) | (((uint8_t) (x >> 8)) << 16) | (((uint8_t) (x >> 16)) << 8) | ((uint8_t) (x >> 24)))

struct EcoffHDRR {
    uint16_t magic;
    uint16_t vstamp;
    union {
        struct {
            int32_t ilineMax;
            int32_t cbLine;
            int32_t cbLineOffset;
            int32_t idnMax;
            int32_t cbDnOffset;
            int32_t ipdMax;
            int32_t cbPdOffset;
            int32_t isymMax;
            int32_t cbSymOffset;
            int32_t ioptMax;
            int32_t cbOptOffset;
            int32_t iauxMax;
            int32_t cbAuxOffset;
            int32_t issMax;
            int32_t cbSsOffset;
            int32_t issExtMax;
            int32_t cbSsExtOffset;
            int32_t ifdMax;
            int32_t cbFdOffset;
            int32_t crfd;
            int32_t cbRfdOffset;
            int32_t iextMax;
            int32_t cbExtOffset;
        };
        int32_t hdrr_ints[MDEBUG_INTS_SIZE];
    };
};

enum EcoffSt {
    ST_STATIC = 2,
    ST_PROC = 6,
    ST_END = 8,
    ST_FILE = 11,
    ST_STATICPROC = 14
};

enum EcoffSc {
};

struct EcoffSYMR {
    int32_t iss;
    int32_t value;
    uint32_t type_index_chunk;

    EcoffSt get_st() const {
        return (EcoffSt) ((type_index_chunk >> 26) & 0x3F);
    }

    EcoffSc get_sc() const {
        return (EcoffSc) ((type_index_chunk >> 21) & 0x1F);
    }

    int32_t get_index() const {
        return type_index_chunk & 0xFFFFF;
    }
};

void mdebug_parse_header(EcoffHDRR& out_header, const char* mdebug_data, uint64_t mdebug_file_offset) {
    size_t data_offset = 0;

    out_header.magic = BE_TO_LE_16(mdebug_data, data_offset);
    out_header.vstamp = BE_TO_LE_16(mdebug_data, data_offset);

    for (int i = 0; i < MDEBUG_INTS_SIZE; ++i) {
        out_header.hdrr_ints[i] = BE_TO_LE_32(mdebug_data, data_offset);
        if (i > 0 && (i & 1) == 0) {
            out_header.hdrr_ints[i] -= mdebug_file_offset;
        }
    }
}

void mdebug_swap_header(EcoffHDRR& out_header) {
    SWAP_16(out_header.magic);
    SWAP_16(out_header.vstamp);

    for (int i = 0; i < MDEBUG_INTS_SIZE; ++i) {
        SWAP_32(out_header.hdrr_ints[i]);
    }
}

void mdebug_swap_sym(EcoffSYMR& out_sym) {
    SWAP_32(out_sym.iss);
    SWAP_32(out_sym.value);
    SWAP_32(out_sym.type_index_chunk);
}

const char* mdebug_get_ss_str(const EcoffHDRR& header, const char* mdebug_data, int32_t iss) {
    return &mdebug_data[header.cbSsOffset + iss];
}

void mdebug_push_function(N64Recomp::Context& context, const char* sym_name, int32_t sym_value, uint32_t func_size, std::vector<N64Recomp::MDebugFunction>& mdebug_functions) {
    std::string sym_section = "";

    printf("found symbol %s\n", sym_name);
    fflush(stdout);

    for (auto section : context.sections) {
        if (!section.executable) {
            continue;
        }

        if (sym_value >= section.ram_addr && sym_value < section.ram_addr + section.size) {
            sym_section = section.name;
            break;
        }
    }

    if (sym_section == "") {
        fmt::print(stderr, "[Warn] Section not found for mdebug symbol {}\n", sym_name);
    }

    mdebug_functions.push_back({sym_name, sym_section, static_cast<uint32_t>(sym_value), func_size});
}

bool N64Recomp::Context::from_mdebug_section(N64Recomp::Context& context, const char* mdebug_data, uint64_t mdebug_file_offset, std::vector<N64Recomp::MDebugFunction>& mdebug_functions) {
    EcoffHDRR header;
    mdebug_parse_header(header, mdebug_data, mdebug_file_offset);

    std::unordered_map<int32_t, EcoffSYMR*> iss_to_sym;
    std::unordered_map<int32_t, uint32_t> iss_to_func_size;

    std::vector<EcoffSYMR> all_syms;
    const EcoffSYMR* syms_ptr = reinterpret_cast<const EcoffSYMR*>(&mdebug_data[header.cbSymOffset]);

    // Param 2 is the pointer to the syms + the count of symbols,
    // not the count of symbol bytes:
    all_syms.assign(syms_ptr, syms_ptr + header.isymMax);

    for (EcoffSYMR& sym : all_syms) {
        mdebug_swap_sym(sym);

        if (sym.get_st() == ST_STATICPROC) {
            iss_to_sym[sym.iss] = &sym;
            printf("add sym %s with iss %d\n", mdebug_get_ss_str(header, mdebug_data, sym.iss), sym.iss);
            fflush(stdout);
        } else if (sym.get_st() == ST_END && iss_to_sym.find(sym.iss) != iss_to_sym.end()) {
            const EcoffSYMR* orig_sym = iss_to_sym[sym.iss];
            mdebug_push_function(context, mdebug_get_ss_str(header, mdebug_data, orig_sym->iss), orig_sym->value, sym.value, mdebug_functions);
        }
    }

    return true;
}