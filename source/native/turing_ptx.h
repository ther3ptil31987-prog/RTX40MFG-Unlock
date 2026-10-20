#pragma once
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// PTX ISA: matrix-fragments-for-mma-m16n8k8 / m16n8k16,
// half-precision-floating-point-instructions-min, and data-movement-and-conversion-instructions-cvt.
// Only the witnessed FP16 forms are lowered. Unknown qualifiers fail closed.
namespace turing_ptx
{
struct Counts { unsigned mma = 0, minmax = 0, pack = 0; };
inline bool Register(std::string_view s) noexcept
{
    if (s.size() < 3 || s[0] != '%') return false;
    for (char c : s.substr(1)) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}
inline bool Lower(std::string& text, Counts* counts = nullptr)
{
    // Transactional: a rejected input is never partially rewritten.
    std::string source = text;
    Counts found{};
    if (source.find("%mfg_t") != std::string::npos) return false;
    size_t target = source.find(".target sm_89");
    if (target == std::string::npos) target = source.find(".target sm_86");
    if (target == std::string::npos || source.find(".target", target + 1) != std::string::npos) return false;
    source.replace(target, 13, ".target sm_75");
    std::string output;
    output.reserve(source.size() * 2);
    for (size_t cursor = 0; cursor < source.size();)
    {
        size_t start = source.find_first_of("mc", cursor);
        if (start == std::string::npos) { output.append(source, cursor); break; }
        auto is = [&](std::string_view s) { return source.compare(start, s.size(), s) == 0; };
        const bool mma = is("mma.sync.aligned.m16n8k16.");
        const bool pack = is("cvt.rn.f16x2.f32");
        const bool mm = is("min.f16") || is("max.f16");
        if (!mma && !pack && !mm) { output.append(source, cursor, start + 1 - cursor); cursor = start + 1; continue; }
        // Predicated forms need a different block placement; reject rather than dropping the predicate.
        size_t line = source.find_last_of(";{}\n", start);
        if (source.substr(line == std::string::npos ? 0 : line + 1,
                start - (line == std::string::npos ? 0 : line + 1)).find('@') != std::string::npos) return false;
        const size_t space = source.find_first_of(" \t\r\n", start);
        const size_t end = source.find(';', start);
        if (space == std::string::npos || end == std::string::npos || space >= end) return false;
        const std::string opcode = source.substr(start, space - start);
        const bool packed = opcode == "min.f16x2" || opcode == "max.f16x2";
        if ((mma && opcode != "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16")
            || (pack && opcode != "cvt.rn.f16x2.f32")
            || (mm && !packed && opcode != "min.f16" && opcode != "max.f16")) return false;
        std::vector<std::string> args;
        std::vector<unsigned> groups;
        for (size_t p = space; p < end;)
        {
            if (source[p] == '{') groups.push_back(static_cast<unsigned>(args.size()));
            if (source[p] == '%')
            {
                size_t last = p + 1;
                while (last < end && (std::isalnum(static_cast<unsigned char>(source[last])) || source[last] == '_')) ++last;
                args.push_back(source.substr(p, last - p)); p = last;
            }
            else if (std::isspace(static_cast<unsigned char>(source[p])) || source[p] == ',' || source[p] == '{' || source[p] == '}') ++p;
            else return false;
        }
        for (auto& a : args) if (!Register(a)) return false;
        output.append(source, cursor, start - cursor);
        if (mma)
        {
            if (args.size() != 10 || groups != std::vector<unsigned>{0,2,6,8}) return false;
            // Each k8 A fragment contains the two row groups. B's first/second
            // register supplies k[0:7]/k[8:15]. A temporary protects aliases
            // between D and the second half of A/B/C.
            output += "{ .reg .b32 %mfg_t0, %mfg_t1;\n";
            const std::string op = "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 ";
            output += op + "{%mfg_t0,%mfg_t1},{"+args[2]+","+args[3]+"},{"+args[6]+"},{"+args[8]+","+args[9]+"};\n";
            output += op + "{"+args[0]+","+args[1]+"},{"+args[4]+","+args[5]+"},{"+args[7]+"},{%mfg_t0,%mfg_t1};\n}";
            ++found.mma;
        }
        else if (pack)
        {
            if (args.size() != 3 || !groups.empty()) return false;
            // PTX cvt.f16x2 puts a in the HIGH half and b in the LOW half.
            output += "{ .reg .b16 %mfg_t0, %mfg_t1;\n"
                "cvt.rn.f16.f32 %mfg_t1,"+args[1]+";\ncvt.rn.f16.f32 %mfg_t0,"+args[2]+";\nmov.b32 "+args[0]+",{%mfg_t0,%mfg_t1};\n}";
            ++found.pack;
        }
        else
        {
            if (args.size() != 3 || !groups.empty()) return false;
            output += "{ .reg .b16 %mfg_t0, %mfg_t1, %mfg_t2, %mfg_t3; .reg .f32 %mfg_ta, %mfg_tb;\n";
            if (packed) output += "mov.b32 {%mfg_t0,%mfg_t1},"+args[1]+";\nmov.b32 {%mfg_t2,%mfg_t3},"+args[2]+";\n";
            else output += "mov.b16 %mfg_t0,"+args[1]+";\nmov.b16 %mfg_t2,"+args[2]+";\n";
            const std::string op = opcode.substr(0,3) + ".f32 ";
            for (unsigned lane = 0; lane < (packed ? 2u : 1u); ++lane)
            {
                const auto a = "%mfg_t" + std::to_string(lane), b = "%mfg_t" + std::to_string(lane + 2);
                output += "cvt.f32.f16 %mfg_ta,"+a+";\ncvt.f32.f16 %mfg_tb,"+b+";\n";
                output += op+"%mfg_ta,%mfg_ta,%mfg_tb;\ncvt.rn.f16.f32 "+a+",%mfg_ta;\n";
            }
            output += packed ? "mov.b32 "+args[0]+",{%mfg_t0,%mfg_t1};\n}" : "mov.b16 "+args[0]+",%mfg_t0;\n}";
            ++found.minmax;
        }
        cursor = end + 1;
    }
    for (const char* blocked : {"m16n8k16", "min.f16", "max.f16", "cvt.rn.f16x2", "cp.async", "mma.sp", ".bf16", ".tf32", ".e4m3", ".e5m2", "wgmma", "tcgen05"})
        if (output.find(blocked) != std::string::npos) return false;
    for (const char* prefix : {"min.", "max.", "cvt."})
        for (size_t at = output.find(prefix); at != std::string::npos; at = output.find(prefix, at + 1))
        {
            const auto op = output.substr(at, output.find_first_of(" \t\r\n", at) - at);
            if (op.find(prefix == std::string_view("cvt.") ? "f16x2" : "f16") != std::string::npos) return false;
        }
    text.swap(output);
    if (counts) *counts = found;
    return true;
}
}
