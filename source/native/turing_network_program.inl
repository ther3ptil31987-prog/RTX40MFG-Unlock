// Included in ampere_gpu's private namespace, after the fatbin builder and pointer publisher.
// The RTX 30 path never enters this code. No provider RVA or full-file hash is a discovery key.
struct TuringNetworkProgram
{
    using CreateKernel = int(__fastcall*)(void*, const void*, uint32_t, const char*, uint32_t,
        void**, uint32_t, uint32_t, uint32_t, uint64_t);
    struct Change { uintptr_t slot = 0, original = 0, replacement = 0; DWORD protection = 0; };
    struct Image
    {
        uintptr_t source = 0;
        uint32_t sourceBytes = 0;
        ampere_cuda_program::Entry name{};
        void* allocation = nullptr;
    };
    HMODULE module = nullptr;
    std::array<uintptr_t, 2> cubinVtables{};
    std::array<uintptr_t, 2> smQueries{};
    std::array<Change, 4> changes{};
    ampere_native_cache::CompleteProgram payloads;
    std::vector<ampere_cuda_program::Entry> names;
    std::vector<Image> images;
    std::array<std::pair<uintptr_t,uint32_t>, kMaximumKernelCount + 1> passthrough{};
    size_t passthroughCount = 0;
    TuringNetworkProgram* next = nullptr;
    bool retained = false;
    ~TuringNetworkProgram() { for (auto& i : images) if (i.allocation) VirtualFree(i.allocation, 0, MEM_RELEASE); }
};
std::atomic<TuringNetworkProgram*> gTuringNetworks{nullptr};
// Once any pointer can be observed, the callbacks and their storage live until process exit.
std::array<std::shared_ptr<TuringNetworkProgram>, 16> gRetainedTuringNetworks;
size_t gRetainedTuringCount = 0;
bool TuringBytesEqual(uintptr_t at, const void* expected, size_t size) noexcept
{
    __try { return memcmp(reinterpret_cast<void*>(at),expected,size)==0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool TuringOwned(HMODULE module, uintptr_t at, size_t bytes, bool code = false) noexcept
{
    uint32_t size = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    if (!ImageSize(module, size) || at < base || bytes > size || at - base > size - bytes) return false;
    for (uintptr_t p = at; p < at + bytes;)
    {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) != sizeof(m)
            || m.AllocationBase != module || m.Type != MEM_IMAGE || m.State != MEM_COMMIT
            || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (code && m.Protect != PAGE_EXECUTE_READ && m.Protect != PAGE_EXECUTE) return false;
        p = (std::min)(at + bytes, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize);
    }
    return true;
}
uintptr_t TuringVtable(HMODULE module, const char* name)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uint32_t size = 0;
    if (!ImageSize(module, size) || size > 256u * 1024u * 1024u) return 0;
    std::string type = std::string(".?AV") + name + "@@";
    uintptr_t descriptor = 0, col = 0, vtable = 0;
    auto scan = [&](size_t stride, auto visitor) {
        for (uintptr_t p = base; p < base + size;)
        {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof(m)) != sizeof(m)) return;
            const uintptr_t end = (std::min)(base + size, reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize);
            if (m.AllocationBase == module && m.Type == MEM_IMAGE && m.State == MEM_COMMIT
                && (m.Protect == PAGE_READONLY || m.Protect == PAGE_READWRITE || m.Protect == PAGE_WRITECOPY))
                for (uintptr_t a = (p + stride - 1) & ~(stride - 1); a + 128 < end; a += stride) visitor(a);
            p = end;
        }
    };
    unsigned matches = 0;
    if(type.size()+1>128)return 0;
    scan(1, [&](uintptr_t p) { if (TuringBytesEqual(p,type.c_str(),type.size()+1)) { descriptor = p - 16; ++matches; } });
    if (matches != 1 || descriptor < base) return 0;
    matches = 0;
    scan(4, [&](uintptr_t p) {
        std::array<uint8_t,24> b{};
        if (!SafeCopy(b.data(),reinterpret_cast<void*>(p),b.size()))return;
        if (ReadU32(b.data()) == 1 && ReadU32(b.data()+4) == 0 && ReadU32(b.data()+8) == 0
            && ReadU32(b.data()+12) == descriptor - base && ReadU32(b.data()+20) == p - base) { col = p; ++matches; }
    });
    if (matches != 1) return 0;
    matches = 0;
    scan(8, [&](uintptr_t p) { uintptr_t value=0;if (SafeRead(p,value)&&value==col) { vtable = p + 8; ++matches; } });
    if (matches != 1 || !IsReadOnlyImageAddress(module, vtable)) return 0;
    for (unsigned i = 0; i < 9; ++i)
    {
        uintptr_t function=0;
        if (!SafeRead(vtable+i*8,function)||!TuringOwned(module,function,1,true))return 0;
    }
    return vtable;
}
struct TuringInstruction { uintptr_t at; std::vector<uint8_t> bytes; hde64s decoded; };
bool TuringDecode(HMODULE module, uintptr_t start, std::vector<TuringInstruction>& instructions)
{
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (!TuringOwned(module, start, 16, true)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.Size || dir.Size % sizeof(RUNTIME_FUNCTION) || !TuringOwned(module, base + dir.VirtualAddress, dir.Size)) return false;
    const auto* funcs = reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress);
    uintptr_t end = 0;
    for (size_t i = 0; i < dir.Size / sizeof(RUNTIME_FUNCTION); ++i)
        if (base + funcs[i].BeginAddress == start) { if (end) return false; end = base + funcs[i].EndAddress; }
    if (end <= start || end - start > 0x10000 || !TuringOwned(module, start, end - start, true)) return false;
    for (uintptr_t at = start; at < end;)
    {
        std::array<uint8_t, 16> b{};
        const size_t available = (std::min)(size_t{16}, static_cast<size_t>(end-at));
        if (!SafeCopy(b.data(), reinterpret_cast<void*>(at), available)) return false;
        hde64s h{}; hde64_disasm(b.data(), &h);
        if (!h.len || h.len > available || h.flags & F_ERROR) return false;
        instructions.push_back({at, {b.begin(), b.begin()+h.len}, h}); at += h.len;
    }
    return !instructions.empty();
}
struct TuringLoad { uintptr_t data = 0, name = 0, helper = 0; uint32_t bytes = 0; size_t imageInstruction = 0, sizeInstruction = 0; };
// Decode the complete GetSM graph with bounded direct branches. This also works
// for leaf methods without unwind metadata; no end is guessed from padding.
bool TuringDecodeSm(HMODULE module, uintptr_t start, std::vector<TuringInstruction>& code)
{
    constexpr size_t limit=256;
    std::array<bool,limit> starts{},occupied{};
    std::vector<size_t> pending{0};
    unsigned returns=0;
    while(!pending.empty())
    {
        size_t at=pending.back();pending.pop_back();
        for(;;)
        {
            if(at>=limit)return false;
            if(starts[at])break;
            if(occupied[at]||!TuringOwned(module,start+at,16,true))return false;
            std::array<uint8_t,16> bytes{};
            if(!SafeCopy(bytes.data(),reinterpret_cast<void*>(start+at),bytes.size()))return false;
            hde64s h{};hde64_disasm(bytes.data(),&h);
            if(!h.len||h.len>limit-at||h.flags&F_ERROR||h.opcode==0xe8||h.opcode==0xff)return false;
            for(size_t k=0;k<h.len;++k)if(occupied[at+k])return false;else occupied[at+k]=true;
            starts[at]=true;code.push_back({start+at,{bytes.begin(),bytes.begin()+h.len},h});
            if(h.opcode==0xc3){++returns;break;}
            const bool shortConditional=h.opcode>=0x70&&h.opcode<=0x7f;
            const bool longConditional=h.opcode==0x0f&&h.opcode2>=0x80&&h.opcode2<=0x8f;
            const bool jump=h.opcode==0xe9||h.opcode==0xeb;
            if(shortConditional||longConditional||jump)
            {
                const int64_t relative=(h.flags&F_IMM8)?static_cast<int8_t>(h.imm.imm8):static_cast<int32_t>(h.imm.imm32);
                const int64_t target=static_cast<int64_t>(at)+h.len+relative;
                if(target<0||target>=limit)return false;
                pending.push_back(static_cast<size_t>(target));
                if(jump)break;
            }
            at+=h.len;
        }
    }
    return returns>0;
}
bool TuringLoads(HMODULE module, const std::vector<TuringInstruction>& code, std::vector<TuringLoad>& loads, bool ptx)
{
    for (size_t i = 0; i < code.size(); ++i)
    {
        const auto& b = code[i].bytes;
        if (b.size() != 7 || b[0] != 0x48 || b[1] != 0x8d || b[2] != 0x15) continue;
        const uintptr_t data = code[i].at + 7 + static_cast<int32_t>(ReadU32(b.data()+3));
        uint32_t magic = 0;
        if (!SafeRead(data, magic) || (magic != 0xBA55ED50 && magic != 0x464c457f)) continue;
        if ((magic == 0xBA55ED50) != ptx || !TuringOwned(module, data, 64)) return false;
        TuringLoad l{}; l.data = data; l.imageInstruction = i;
        // The compiler places r8d/r9 on either side of rdx. All three arguments
        // must belong to the same direct call, with no intervening call.
        size_t first = i > 10 ? i - 10 : 0, last = (std::min)(code.size(), i + 12);
        for (size_t j = i; j > first; --j) if (code[j-1].decoded.opcode == 0xe8) { first = j; break; }
        for (size_t j = i+1; j < last; ++j) if (code[j].decoded.opcode == 0xe8) { last = j+1; break; }
        for (size_t j = first; j < last; ++j)
        {
            const auto& v = code[j].bytes;
            if (v.size()==6 && v[0]==0x41 && v[1]==0xb8) { l.bytes=ReadU32(v.data()+2); l.sizeInstruction=j; }
            if (v.size()==7 && v[0]==0x4c && v[1]==0x8d && v[2]==0x0d) l.name=code[j].at+7+static_cast<int32_t>(ReadU32(v.data()+3));
            if (j > i && v.size()==5 && v[0]==0xe8) { l.helper=code[j].at+5+static_cast<int32_t>(ReadU32(v.data()+1)); break; }
        }
        if (!l.bytes || l.bytes > kMaximumFatbinBytes || !l.name || !l.helper
            || !TuringOwned(module, data, l.bytes) || !TuringOwned(module, l.name, 96)
            || !TuringOwned(module, l.helper, 16, true)) return false;
        loads.push_back(l);
    }
    return !loads.empty() && loads.size() <= 64;
}
bool TuringSameLoaders(const std::vector<TuringInstruction>& a, const std::vector<TuringInstruction>& b,
    const std::vector<TuringLoad>& la, const std::vector<TuringLoad>& lb)
{
    if (a.size()!=b.size() || la.size()!=lb.size()) return false;
    for (size_t k=0; k<la.size(); ++k)
        if (la[k].imageInstruction != lb[k].imageInstruction || la[k].sizeInstruction != lb[k].sizeInstruction
            || la[k].name != lb[k].name || la[k].helper != lb[k].helper) return false;
    for (size_t i=0; i<a.size(); ++i)
    {
        auto x=a[i].bytes, y=b[i].bytes;
        for (size_t k=0; k<la.size(); ++k)
        {
            if (i==la[k].imageInstruction) { std::fill(x.begin()+3,x.end(),0); std::fill(y.begin()+3,y.end(),0); }
            if (i==la[k].sizeInstruction) { std::fill(x.begin()+2,x.end(),0); std::fill(y.begin()+2,y.end(),0); }
        }
        const auto& h=a[i].decoded;
        // Every other RIP operand and direct call must resolve to the same
        // provider-owned object/callee. Internal branches retain identical offsets.
        if (h.modrm_mod==0 && h.modrm_rm==5 && h.flags & F_DISP32)
        {
            const size_t off=x.size()-4;
            if (x!=y && a[i].at+x.size()+static_cast<int32_t>(ReadU32(x.data()+off))
                    != b[i].at+y.size()+static_cast<int32_t>(ReadU32(y.data()+off))) return false;
            std::fill(x.begin()+off,x.end(),0); std::fill(y.begin()+off,y.end(),0);
        }
        if (h.opcode==0xe8 && x.size()==5 && y.size()==5)
        {
            if (a[i].at+5+static_cast<int32_t>(ReadU32(x.data()+1)) != b[i].at+5+static_cast<int32_t>(ReadU32(y.data()+1))) return false;
            std::fill(x.begin()+1,x.end(),0); std::fill(y.begin()+1,y.end(),0);
        }
        if (x!=y) return false;
    }
    return true;
}
bool TuringNetworkCurrent(const TuringNetworkProgram& program, bool published) noexcept
{
    for (size_t api=0; api<2; ++api)
    {
        uintptr_t query=0;
        if (!SafeRead(program.cubinVtables[api]+8*8,query) || query!=program.smQueries[api]) return false;
    }
    for (auto& c : program.changes)
    {
        uintptr_t value=0;
        if (!SafeRead(c.slot,value) || value!=(published?c.replacement:c.original)
            || !IsReadOnlyImageAddress(program.module,c.slot)
            || !protected_pointer::ProtectionMatches(c.slot,c.protection,sizeof(void*))) return false;
    }
    if (program.images.size()!=program.payloads.size() || program.images.size()!=program.names.size()) return false;
    for (size_t i=0; i<program.images.size(); ++i)
    {
        MEMORY_BASIC_INFORMATION m{};
        const auto allocation=program.images[i].allocation;
        if (!allocation || VirtualQuery(allocation,&m,sizeof(m))!=sizeof(m)
            || m.AllocationBase!=allocation || m.BaseAddress!=allocation || m.Type!=MEM_PRIVATE
            || m.State!=MEM_COMMIT || m.Protect!=PAGE_READONLY || m.RegionSize<program.payloads[i].size()) return false;
    }
    return true;
}
int __fastcall TuringCreateKernel(void* self, const void* data, uint32_t bytes, const char* name,
    uint32_t flags, void** handle, uint32_t x, uint32_t y, uint32_t z, uint64_t shared)
{
    uintptr_t vt=0;
    if (!SafeRead(reinterpret_cast<uintptr_t>(self),vt)) return -1;
    for (auto* p=gTuringNetworks.load(std::memory_order_acquire); p; p=p->next)
        for (size_t api=0; api<2; ++api) if (vt==p->cubinVtables[api])
        {
            if (!TuringNetworkCurrent(*p,true)) return -1;
            // Each instance must still identify itself as SM75. A second device
            // or an explicit provider SM override cannot use this translation.
            using GetSm=uint32_t(__fastcall*)(void*);
            if (reinterpret_cast<GetSm>(p->smQueries[api])(self)!=75) return -1;
            auto original=reinterpret_cast<TuringNetworkProgram::CreateKernel>(p->changes[api].original);
            for (size_t i=0; i<p->images.size(); ++i) if (reinterpret_cast<uintptr_t>(data)==p->images[i].source)
            {
                std::array<char,96> observed{};
                const size_t nameBytes=strlen(p->names[i].data())+1;
                if (flags!=1 || bytes!=p->images[i].sourceBytes || !SafeCopy(observed.data(),name,nameBytes)
                    || memcmp(observed.data(),p->names[i].data(),nameBytes)) return -1;
                return original(self,p->images[i].allocation,static_cast<uint32_t>(p->payloads[i].size()),name,flags,handle,x,y,z,shared);
            }
            // General kernels and the font have already been replaced. Refuse
            // any unrecognized native/SM89 image instead of sending it to Turing.
            bool known=false;
            for (size_t i=0; i<p->passthroughCount; ++i)
                if (p->passthrough[i].first==reinterpret_cast<uintptr_t>(data) && p->passthrough[i].second==bytes) known=true;
            if (!known) return -1;
            std::array<uint8_t,80> header{};
            if (!SafeCopy(header.data(),data,header.size()) || ReadU32(header.data())!=0xBA55ED50
                || ReadU16(header.data()+6)!=16 || ReadU64(header.data()+8)+16>bytes
                || ReadU16(header.data()+16)!=1 || ReadU32(header.data()+44)!=75) return -1;
            return original(self,data,bytes,name,flags,handle,x,y,z,shared);
        }
    return -1;
}
std::shared_ptr<TuringNetworkProgram> PrepareTuringNetwork(HMODULE module)
{
    auto p=std::make_shared<TuringNetworkProgram>(); p->module=module;
    // No vector growth may throw between a raw allocation and its ownership
    // transfer below. There are exactly 25 + 14 validated network images.
    p->images.reserve(39);p->payloads.reserve(39);p->names.reserve(39);
    for (size_t api=0; api<2; ++api)
    {
        const auto vt=TuringVtable(module,api?"NGXCubinVulkan":"NGXCubinD3D12");
        uintptr_t fn=0;
        if (!vt || !SafeRead(vt+12*8,fn) || !TuringOwned(module,fn,16,true)) return {};
        p->cubinVtables[api]=vt;
        p->changes[api]={vt+12*8,fn,reinterpret_cast<uintptr_t>(&TuringCreateKernel)};
        if (!SafeRead(vt+8*8,p->smQueries[api])) return {};
        std::vector<TuringInstruction> query;
        if (!TuringDecodeSm(module,p->smQueries[api],query)) return {};
        auto instruction=[&](uintptr_t at)->const TuringInstruction* {
            for (const auto& ins:query) if(ins.at==at)return &ins;return nullptr;
        };
        auto zeroReturns=[&](uintptr_t at,uint32_t value)->uintptr_t {
            const auto* branch=instruction(at);
            if(!branch||branch->decoded.opcode!=0x74||!(branch->decoded.flags&F_IMM8))return 0;
            const auto* result=instruction(at+branch->bytes.size()+static_cast<int8_t>(branch->decoded.imm.imm8));
            return result&&result->decoded.opcode==0xb8&&result->decoded.imm.imm32==value?at+branch->bytes.size():0;
        };
        unsigned maps=0;
        for(const auto& ins:query)
        {
            const auto& h=ins.decoded;
            // sub ecx, 0x160; jz return_75; sub ecx, 0x10; jz return_86.
            if(h.opcode!=0x81||h.modrm!=0xe9||h.imm.imm32!=0x160)continue;
            const auto next=zeroReturns(ins.at+ins.bytes.size(),75);
            const auto* delta=instruction(next);
            if(delta&&delta->decoded.opcode==0x83&&delta->decoded.modrm==0xe9&&delta->decoded.imm.imm8==0x10
                &&zeroReturns(delta->at+delta->bytes.size(),86))++maps;
        }
        if(maps!=1)return {};
    }
    for (size_t net=0; net<2; ++net)
    {
        const char* name=net?"Flatten_EndpointDL2Net":"Flatten_EndpointDL1Net";
        const auto vt=TuringVtable(module,name), pt=TuringVtable(module,(std::string(name)+"_89_PTX").c_str());
        if (!vt || !pt) return {};
        std::array<uintptr_t,9> nativeTable{},ptxTable{};
        if(!SafeCopy(nativeTable.data(),reinterpret_cast<void*>(vt),sizeof(nativeTable))
            ||!SafeCopy(ptxTable.data(),reinterpret_cast<void*>(pt),sizeof(ptxTable)))return {};
        for (size_t i=0; i<9; ++i) if (i!=1 && i!=4 && nativeTable[i]!=ptxTable[i]) return {};
        const auto native=nativeTable[4], loader=ptxTable[4];
        std::vector<TuringInstruction> a,b; std::vector<TuringLoad> la,lb;
        if (!TuringDecode(module,native,a) || !TuringDecode(module,loader,b)
            || !TuringLoads(module,a,la,false) || !TuringLoads(module,b,lb,true)
            || !TuringSameLoaders(a,b,la,lb) || lb.size()!=(net?14u:25u)) return {};
        // The shared helper must call slot 12 (0x60) of the NGXCubin interface,
        // with this = network->cubin at +8. This establishes the substitution ABI.
        std::vector<TuringInstruction> helper;
        if (!TuringDecode(module,lb.front().helper,helper)) return {};
        unsigned cubinLoads=0, slotLoads=0;
        for (const auto& ins:helper)
        {
            const auto& h=ins.decoded;
            if (h.opcode==0x8b && h.rex_w && h.modrm_mod==1 && h.disp.disp8==8 && h.modrm_rm==1) ++cubinLoads;
            if (h.opcode==0x8b && h.rex_w && h.modrm_mod==1 && h.disp.disp8==0x60) ++slotLoads;
        }
        if (cubinLoads!=1 || slotLoads!=1) return {};
        p->changes[net+2]={vt+4*8,native,loader};
        for (auto& l : lb)
        {
            TuringNetworkProgram::Image image{}; image.source=l.data; image.sourceBytes=l.bytes;
            if (!SafeCopy(image.name.data(),reinterpret_cast<void*>(l.name),image.name.size()-1)
                || !memchr(image.name.data(),0,image.name.size()-1)) return {};
            Sm89PtxMetadata meta{};
            if (!FindUniqueSm89Ptx(l.data,l.bytes,meta)) return {};
            const size_t capacity=meta.outerHeaderSize+meta.imageHeaderSize+meta.rawPtxBytes*16+4096;
            if (capacity>kMaximumFatbinBytes) return {};
            std::vector<uint8_t> payload(capacity),scratch(meta.compressedSize);
            uint32_t size=0; StripFailure reason{};
            if (!BuildAmpereSm86Fatbin(l.data,l.bytes,meta,payload.data(),payload.size(),scratch.data(),scratch.size(),size,reason)) return {};
            payload.resize(size);
            image.allocation=VirtualAlloc(nullptr,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
            if (!image.allocation) return {};
            memcpy(image.allocation,payload.data(),size);
            p->images.push_back(image); // Own the allocation before any fallible operation.
            DWORD old=0;
            if (!VirtualProtect(image.allocation,size,PAGE_READONLY,&old)
                || memcmp(image.allocation,payload.data(),size)) return {};
            p->payloads.push_back(std::move(payload)); p->names.push_back(image.name);
        }
    }
    for (auto& c:p->changes) if (!protected_pointer::QueryProtection(c.slot,c.protection,sizeof(void*))) return {};
    return TuringNetworkCurrent(*p,false)?p:nullptr;
}
bool WriteTuringNetwork(std::shared_ptr<TuringNetworkProgram> p, bool publish, protected_pointer::ProtectMemoryFn protect) noexcept
{
    if (!p) return true;
    if (publish && !p->retained)
    {
        if (gRetainedTuringCount==gRetainedTuringNetworks.size() || !TuringNetworkCurrent(*p,false)) return false;
        gRetainedTuringNetworks[gRetainedTuringCount++]=p;
        p->next=gTuringNetworks.load(std::memory_order_acquire); p->retained=true;
        gTuringNetworks.store(p.get(),std::memory_order_release);
    }
    bool success=true;
    for (size_t k=0; k<p->changes.size(); ++k)
    {
        const auto& c=p->changes[publish?k:p->changes.size()-1-k];
        uintptr_t value=0; const auto expected=publish?c.original:c.replacement, replacement=publish?c.replacement:c.original;
        const bool read=SafeRead(c.slot,value);
        if (!read || (value!=replacement || publish) && (value!=expected || !PublishPointer(c.slot,expected,replacement,protect).success))
        { success=false; if (publish) break; }
        if (!publish) protected_pointer::RestoreProtectionWithRetry(reinterpret_cast<void*>(c.slot),sizeof(void*),c.protection,protect);
    }
    if (publish && !success) WriteTuringNetwork(p,false,protect);
    return success && TuringNetworkCurrent(*p,publish);
}
