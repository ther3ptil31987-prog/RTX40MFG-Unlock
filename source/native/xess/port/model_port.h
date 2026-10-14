// Exact-build, process-local research only. Restore metadata after context teardown.
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <unordered_map>

class ModelPort {
    struct GraphNode { uint32_t op; std::string key; bool flag; };
    static_assert(sizeof(std::string) == 32 && sizeof(GraphNode) == 48);
    struct VectorView { const void* begin; const void* end; const void* capacity; };
    struct Edit { uintptr_t address; std::vector<unsigned char> before, after; };
    std::vector<Edit> edits;
    std::vector<GraphNode> graph;
    VectorView graphView{};
    template<size_t N> struct Descriptor { std::string name; std::array<unsigned char,N> body; };
    std::unordered_map<std::string,Descriptor<0xe0>> reconstruction;
    std::unordered_map<std::string,Descriptor<0xd8>> output;
    std::vector<std::string> outputInputs;
    bool active{};
    static std::vector<unsigned char> read(uintptr_t address, size_t size) {
        if (!address || !size || size > 1000000) throw std::runtime_error("port_read_bounds");
        std::vector<unsigned char> bytes(size); SIZE_T got{};
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), bytes.data(), size, &got) || got != size)
            throw std::runtime_error("port_unreadable");
        return bytes;
    }
    template<class T> static T value(uintptr_t p) {
        auto bytes = read(p, sizeof(T)); T v; memcpy(&v, bytes.data(), sizeof(T)); return v;
    }
    static std::string name(uintptr_t p) {
        auto size = value<size_t>(p + 16), capacity = value<size_t>(p + 24);
        if (size == 0 || size > 128 || capacity < size || capacity > 65536) throw std::runtime_error("port_string_shape");
        auto bytes = read(capacity >= 16 ? value<uintptr_t>(p) : p, size);
        return std::string(bytes.begin(), bytes.end());
    }
    static std::map<std::string, uintptr_t> table(uintptr_t p, size_t expected = 0) {
        if (value<float>(p) != 1.0f) throw std::runtime_error("port_map_shape");
        auto head = value<uintptr_t>(p + 8), count = value<size_t>(p + 16);
        if (!count || count > 1024 || (expected && count != expected)) throw std::runtime_error("port_map_count");
        std::map<std::string, uintptr_t> out;
        auto node = value<uintptr_t>(head), prior = head;
        while (node != head) {
            if (out.size() >= count || value<uintptr_t>(node + 8) != prior || !out.emplace(name(node + 16), node).second)
                throw std::runtime_error("port_map_links");
            prior = node; node = value<uintptr_t>(node);
        }
        if (out.size() != count || value<uintptr_t>(head + 8) != prior) throw std::runtime_error("port_map_length");
        return out;
    }
    template<class T> void plan(uintptr_t p, T expected, T replacement) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (value<T>(p) != expected) throw std::runtime_error("port_expected_value");
        if (expected == replacement) return;
        for (const auto& e : edits)
            if (p < e.address + e.after.size() && e.address < p + sizeof(T)) throw std::runtime_error("port_overlapping_edits");
        Edit e{p, read(p,sizeof(T)), std::vector<unsigned char>(sizeof(T))};
        memcpy(e.after.data(), &replacement, sizeof(T)); edits.push_back(std::move(e));
    }
    static void write(const Edit& e, bool restore) {
        const auto& before = restore ? e.after : e.before;
        const auto& after = restore ? e.before : e.after;
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(e.address), &m, sizeof(m)) != sizeof(m) ||
            m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE || m.Type != MEM_PRIVATE ||
            e.address + after.size() > reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize ||
            read(e.address, before.size()) != before) throw std::runtime_error("port_write_ownership");
        if (after.size()==8 && e.address%8==0) {
            LONG64 expected{},replacement{};
            memcpy(&expected,before.data(),8); memcpy(&replacement,after.data(),8);
            if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(e.address),replacement,expected)!=expected)
                throw std::runtime_error("port_pointer_compare_exchange");
        } else if (after.size()==4 && e.address%4==0) {
            LONG expected{},replacement{};
            memcpy(&expected,before.data(),4); memcpy(&replacement,after.data(),4);
            if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(e.address),replacement,expected)!=expected)
                throw std::runtime_error("port_scalar_compare_exchange");
        } else throw std::runtime_error("port_edit_alignment");
        if (read(e.address, after.size()) != after) throw std::runtime_error("port_write_readback");
    }
public:
    explicit ModelPort(HMODULE module, bool portable, bool reconstructionTexture = true) {
        uintptr_t base = reinterpret_cast<uintptr_t>(module);
        auto models = table(base + xess_layout::Rva(module, 0x15be920), 3);
        const auto donor = models.at("i0"), fallback = models.at("i65");
        if (value<uint32_t>(donor + 0x50) != 15 || value<uint32_t>(fallback + 0x50) != 18 ||
            value<uint32_t>(models.at("i62") + 0x50) != 17 ||
            name(donor + 0x30) != "468cb740f9dd46bc919741718b1fe210") throw std::runtime_error("port_model_identity");
        for (auto [key,p] : models) if (value<uint32_t>(p+0x50)==16) throw std::runtime_error("port_model_16_exists");
        // The numeric model lookup scans the validated string-keyed catalog.
        // The original Intel record is restored before its static destructor.
        plan<uint32_t>(donor + 0x50, 15, 16);
        if (portable) {
            auto donorTable = [&](unsigned i) { return value<uintptr_t>(donor + 0x78 + i*8); };
            auto fallbackTable = [&](unsigned i) { return value<uintptr_t>(fallback + 0x78 + i*8); };
            if (reconstructionTexture) {
                const auto stage = table(donorTable(3),1).at("i40");
                const auto reference = table(fallbackTable(3),1).at("i66");
                const auto resources = value<uintptr_t>(stage+0xd8);
                const auto referenceResources = value<uintptr_t>(reference+0xd8);
                if (value<uintptr_t>(stage+0xe0)-resources != 5*64 ||
                    value<uintptr_t>(reference+0xe0)-referenceResources != 5*64 ||
                    name(resources) != "i41" || name(referenceResources) != "i41")
                    throw std::runtime_error("port_reconstruction_resource_shape");
                for (size_t off : {40u,44u,48u,52u})
                    if (value<uint32_t>(resources+off) != value<uint32_t>(referenceResources+off))
                        throw std::runtime_error("port_reconstruction_resource_semantics");
                if (value<uint32_t>(referenceResources+32) != 4 ||
                    value<uint32_t>(referenceResources+36) != 0)
                    throw std::runtime_error("port_reconstruction_reference_type");
                // Family 351 now writes exactly the texture-array layout read
                // by family 344, including the i61 residual input. Its trained
                // calculations and four packed uint outputs are unchanged.
                plan<uint32_t>(resources+32, 0, 4);
                plan<uint32_t>(resources+36, 1, 0);
            }
            // Backend scheduling/layout metadata for the shared reconstruction
            // layers. Trained MFG weights remain in the donor's table zero.
            // Registry initializer strides are 0x120 and 0x118: a 32-byte
            // key followed by the named descriptor. Shallow borrowed vectors
            // stay alive in the unmodified fallback catalog throughout use.
            for (auto [key,p] : table(fallbackTable(1),12)) {
                Descriptor<0xe0> d; d.name=name(p+0x30);
                auto bytes=read(p+0x50,d.body.size()); memcpy(d.body.data(),bytes.data(),bytes.size());
                for (size_t offset : {0x58u,0x78u}) {
                    auto s=reinterpret_cast<uintptr_t>(d.body.data()+offset);
                    if (value<size_t>(s+16)==0) continue;
                    auto ref=name(s);
                    if (ref.starts_with("i66::")) {
                        if (value<size_t>(s+24)!=15) throw std::runtime_error("port_inline_reference_shape");
                        memcpy(d.body.data()+offset,"i40",3);
                    }
                }
                reconstruction.emplace(key,std::move(d));
            }
            auto outEntries=table(fallbackTable(2),1);
            auto outSource=outEntries.at("i59");
            Descriptor<0xd8> d; d.name=name(outSource+0x30);
            auto bytes=read(outSource+0x50,d.body.size()); memcpy(d.body.data(),bytes.data(),bytes.size());
            auto inputsBegin=value<uintptr_t>(outSource+0xe0),inputsEnd=value<uintptr_t>(outSource+0xe8);
            if (inputsEnd-inputsBegin!=5*sizeof(std::string)) throw std::runtime_error("port_output_inputs");
            for (auto p=inputsBegin;p<inputsEnd;p+=sizeof(std::string)) {
                auto ref=name(p); if (ref.starts_with("i66::")) ref.replace(0,3,"i40");
                outputInputs.push_back(std::move(ref));
            }
            VectorView inputsView{outputInputs.data(),outputInputs.data()+outputInputs.size(),outputInputs.data()+outputInputs.size()};
            memcpy(d.body.data()+0x90,&inputsView,sizeof(inputsView));
            output.emplace("i59",std::move(d));
            // Verify the host STL matches the decoded release-map ABI before
            // publishing it to the DLL.
            table(reinterpret_cast<uintptr_t>(&reconstruction),12);
            table(reinterpret_cast<uintptr_t>(&output),1);
            plan<uintptr_t>(donor + 0x80, donorTable(1), reinterpret_cast<uintptr_t>(&reconstruction));
            plan<uintptr_t>(donor + 0x88, donorTable(2), reinterpret_cast<uintptr_t>(&output));
            for (unsigned t : {4u,5u,6u,7u}) {
                auto target = table(donorTable(t)); auto reference = table(fallbackTable(t));
                for (auto [key,p] : target) {
                    if (t == 5 && key == "i4") continue; // MFG-specific HLSL stage.
                    std::string refKey = key;
                    if (key == "i18" || key == "i19") refKey = "i17";
                    if (key == "i33" || key == "i34") refKey = "i32";
                    auto r = reference.at(refKey);
                    // Only six decoded scheduling scalars, never flags,
                    // padding, stage indices, channel counts or weights.
                    for (unsigned word = 1; word <= 6; ++word)
                        plan<uint32_t>(p+0x50+word*4, value<uint32_t>(p+0x50+word*4), value<uint32_t>(r+0x50+word*4));
                    // Block010 consumes descriptor+0x5c as the spatial stride
                    // specialization (d98). The CM first-layer builder does
                    // not consume this field. The shared downsampling stages
                    // require the paired HLSL stride, as well as workgroup sizes.
                    if (t==6)
                        plan<uint32_t>(p+0x8c,value<uint32_t>(p+0x8c),value<uint32_t>(r+0x8c));
                    // CM and HLSL encode the intermediate tensors differently.
                    // Port only the two format fields in each original buffer.
                    // Copying a vector's owning pointers aliases fallback
                    // allocations and double-frees them during SDK shutdown.
                    const size_t resourceOffset = t==4 ? 0x98 : t==5 ? 0x78 : t==6 ? 0xa0 : 0x80;
                    const auto a=value<uintptr_t>(p+resourceOffset),b=value<uintptr_t>(p+resourceOffset+8);
                    const auto ra=value<uintptr_t>(r+resourceOffset),rb=value<uintptr_t>(r+resourceOffset+8);
                    if (b<=a || b-a!=rb-ra || (b-a)%64 || b-a>128) throw std::runtime_error("port_tensor_vector_shape");
                    if (value<uintptr_t>(p+resourceOffset+16)<b || value<uintptr_t>(r+resourceOffset+16)<rb)
                        throw std::runtime_error("port_tensor_vector_capacity");
                    for (size_t off=0;off<b-a;off+=64) {
                        if (name(a+off)!=name(ra+off)) throw std::runtime_error("port_tensor_name_mismatch");
                        for (size_t field=40;field<56;field+=4)
                            if (value<uint32_t>(a+off+field)!=value<uint32_t>(ra+off+field))
                                throw std::runtime_error("port_tensor_resource_semantics");
                        // Retain the trailing bytes verbatim; only the flag's
                        // first byte is initialized consistently by this SDK.
                        if (value<unsigned char>(a+off+56)!=value<unsigned char>(ra+off+56))
                            throw std::runtime_error("port_tensor_resource_flag");
                        if (value<uint32_t>(a+off+32)>15 || value<uint32_t>(ra+off+32)>15 ||
                            value<uint32_t>(a+off+36)>1 || value<uint32_t>(ra+off+36)>1)
                            throw std::runtime_error("port_tensor_resource_format");
                        for (size_t field : {32u,36u})
                            plan<uint32_t>(a+off+field,value<uint32_t>(a+off+field),value<uint32_t>(ra+off+field));
                    }
                }
            }
            auto oldView = donorTable(9);
            auto begin = value<uintptr_t>(oldView), end = value<uintptr_t>(oldView+8);
            if (end <= begin || end-begin != 45*sizeof(GraphNode)) throw std::runtime_error("port_graph_shape");
            const std::map<uint32_t,uint32_t> translate{{9,7},{13,10},{12,10},{23,11},{19,21},{20,22},{14,15},{3,1},{4,5}};
            graph.reserve(46);
            for (auto p=begin; p<end; p+=sizeof(GraphNode)) {
                auto op = value<uint32_t>(p); auto key = name(p+8); auto flag = value<unsigned char>(p+40);
                if (flag > 1) throw std::runtime_error("port_graph_flag");
                if (key == "i59") graph.push_back({1,"i61",true});
                graph.push_back({translate.contains(op) ? translate.at(op) : op,key,flag != 0});
            }
            if (graph.size()!=46) throw std::runtime_error("port_graph_length");
            graphView = {graph.data(),graph.data()+graph.size(),graph.data()+graph.size()};
            plan<uintptr_t>(donor+0xc0,oldView,reinterpret_cast<uintptr_t>(&graphView));
        }
        // All modifications happen before the first context and any worker.
        size_t applied = 0;
        try { for (const auto& e : edits) { write(e,false); ++applied; } }
        catch (...) {
            try { while (applied) write(edits[--applied],true); }
            catch (...) { TerminateProcess(GetCurrentProcess(),91); }
            throw;
        }
        active = true;
        logEvent("model_port_applied", "\"donor_model\":15,\"target_model\":16,\"portable_layout\":" +
                 std::string(portable ? "true" : "false") + ",\"edits\":" + std::to_string(edits.size()) +
                 ",\"reconstruction_texture\":" + std::string(reconstructionTexture ? "true" : "false") +
                 ",\"mfg_weights_preserved\":true,\"real_vendor_preserved\":true");
    }
    void restore() {
        if (!active) return;
        for (auto it=edits.rbegin(); it!=edits.rend(); ++it) write(*it,true);
        active = false;
        logEvent("model_port_restored", "\"all_expected_values\":true");
    }
    ~ModelPort() { if (active) { try { restore(); } catch (...) { TerminateProcess(GetCurrentProcess(),92); } } }
};
