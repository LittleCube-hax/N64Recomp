#include <optional>

#include "fmt/format.h"
// #include "fmt/ostream.h"

#include "recompiler/context.h"

#define MDEBUG_INTS_SIZE 23

#define BE_TO_LE_8(x, off) (x[off++])
#define BE_TO_LE_16(x, off) ((x[off++] << 8) | x[off++])
#define BE_TO_LE_32(x, off) ((((uint8_t) (x[off++])) << 24) | (((uint8_t) (x[off++])) << 16) | (((uint8_t) (x[off++])) << 8) | ((uint8_t) (x[off++])))

struct MDebugHDRR {
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
    ST_NIL = 0,
    ST_GLOBAL = 1,
    ST_STATIC = 2,
    ST_PROC = 6,
    ST_END = 8,
    ST_STATICPROC = 14
};

enum EcoffSc {
    SC_NIL = 0,
    SC_TEXT = 1,
    SC_DATA = 2,
    SC_ABS = 5
};

struct MDebugSYMR {
    int32_t iss;
    int32_t value;
    EcoffSt st; // 6 bits
    EcoffSc sc; // 5 bits
    int8_t reserved; // 1 bit
    int32_t index; // 20 bits
    uint32_t size; // custom for recompiler
};

void mdebug_parse_header(MDebugHDRR& out_header, const char* mdebug_data, uint64_t mdebug_file_offset) {
    size_t data_offset = 0;

    out_header.magic = BE_TO_LE_16(mdebug_data, data_offset);
    out_header.vstamp = BE_TO_LE_16(mdebug_data, data_offset);

    for (int i = 0; i < MDEBUG_INTS_SIZE; ++i) {
        out_header.hdrr_ints[i] = BE_TO_LE_32(mdebug_data, data_offset);
    }

    //~ fprintf(stderr, "magic is 0x%02X\n", out_header.magic);
    //~ fprintf(stderr, "ilineMax is 0x%04X and 0x%04X\n", out_header.ilineMax, out_header.hdrr_ints[0]);
    //~ fprintf(stderr, "cbSsOffset is 0x%04X and 0x%04X\n", out_header.cbSsOffset, out_header.hdrr_ints[14]);
}

void mdebug_parse_ss_strs(std::unordered_map<int32_t, std::string>& out_ss_strs, const MDebugHDRR& header, const char* mdebug_data, uint64_t mdebug_file_offset) {
    size_t str_offset = 0;

    for (int i = 0; str_offset < header.issMax; ++i) {
        int32_t curr_iss = str_offset;
        out_ss_strs[curr_iss] = "";
        char c = mdebug_data[header.cbSsOffset - mdebug_file_offset + str_offset];
        while (c != 0) {
            out_ss_strs[curr_iss] += c;
            str_offset += 1;
            c = mdebug_data[header.cbSsOffset - mdebug_file_offset + str_offset];
        }
        str_offset += 1;
        //~ fprintf(stderr, "string: %s\n", out_ss_strs[curr_iss].c_str());
    }
}

void mdebug_parse_sym(MDebugSYMR& out_sym, N64Recomp::Context& context, const char* mdebug_data, uint64_t& offset) {
    out_sym.iss = BE_TO_LE_32(mdebug_data, offset);
    out_sym.value = BE_TO_LE_32(mdebug_data, offset);
    uint32_t sym_chunk = BE_TO_LE_32(mdebug_data, offset);
    out_sym.st = (EcoffSt) ((sym_chunk >> 26) & 0x3F);
    out_sym.sc = (EcoffSc) ((sym_chunk >> 21) & 0x1F);
    out_sym.index = sym_chunk & 0xFFFFF;
}

bool N64Recomp::Context::from_mdebug_section(N64Recomp::Context& context, const char* mdebug_data, uint64_t mdebug_file_offset, std::vector<N64Recomp::MDebugFunction>& mdebug_functions) {
    MDebugHDRR header;
    mdebug_parse_header(header, mdebug_data, mdebug_file_offset);

    std::unordered_map<int32_t, std::string> ss_strs;
    mdebug_parse_ss_strs(ss_strs, header, mdebug_data, mdebug_file_offset);

    size_t sym_offset = header.cbSymOffset - mdebug_file_offset;

    std::vector<MDebugSYMR> syms;

    //~ fprintf(stderr, "isymMax: 0x%08X\n", header.isymMax);

    for (int i = 0; i < header.isymMax; ++i) {
        MDebugSYMR sym{};
        mdebug_parse_sym(sym, context, mdebug_data, sym_offset);

        if (sym.st == ST_STATICPROC) {
            syms.push_back(sym);

            MDebugSYMR end_sym{};
            mdebug_parse_sym(end_sym, context, mdebug_data, sym_offset);
            i += 1;
            sym.size = end_sym.value;

            std::string sym_section = "";

            for (auto section : context.sections) {
                printf("section: %s, addr: 0x%04X, size: 0x%04X\n", section.name.c_str(), section.ram_addr, section.size);
                if (sym.value >= section.ram_addr && sym.value < section.ram_addr + section.size) {
                    //~ printf("section: %s, addr: 0x%04X, size: 0x%04X\n", section.name.c_str(), section.ram_addr, section.size);
                    sym_section = section.name;
                    break;
                }
            }

            if (sym_section == "") {
                fmt::print("Section not found for mdebug symbol %s\n", ss_strs[sym.iss]);
                return false;
            }

            printf("name: %s, section: %s, value: 0x%04X, size: %d\n", ss_strs[sym.iss].c_str(), sym_section.c_str(), (uint32_t) sym.value, sym.size);

            mdebug_functions.push_back({ss_strs[sym.iss], sym_section, static_cast<uint32_t>(sym.value), sym.size});
        }
    }

    return true;
}