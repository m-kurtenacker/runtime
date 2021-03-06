#include <fstream>
#include <sstream>
#include <memory>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>

#include <impala/impala.h>
#include <impala/ast.h>
#include <thorin/world.h>
#include <thorin/transform/codegen_prepare.h>
#include <thorin/be/llvm/llvm.h>

#include "anydsl_runtime.h"
#include "runtime.h"

struct MemBuf : public std::streambuf {
    MemBuf(const char* string, uint32_t size) {
        auto begin = const_cast<char*>(string);
        auto end   = const_cast<char*>(string + size);
        setg(begin, begin, end);
    }
};

static const char runtime_srcs[] = {
#include "runtime_srcs.inc"
};

struct JIT {
    struct Program {
        Program(llvm::ExecutionEngine* engine) : engine(engine) {}
        llvm::ExecutionEngine* engine;
    };

    std::vector<Program> programs;

    JIT() {
        impala::init();
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    }

    int32_t compile(const char* program, uint32_t size, uint32_t opt) {
        std::unique_ptr<llvm::LLVMContext> llvm_context;
        std::unique_ptr<llvm::Module> llvm_module;
        std::string cached_llvm = runtime().load_cache(std::string(program), ".llvm");
        std::string module_name = "jit";
        if (cached_llvm.empty()) {
            bool debug = false;
            assert(opt <= 3);

            impala::Items items;
            MemBuf program_buf(program, size);
            MemBuf runtime_buf(runtime_srcs, sizeof(runtime_srcs));
            std::istream program_is(&program_buf);
            std::istream runtime_is(&runtime_buf);
            impala::parse(items, runtime_is, "runtime");
            impala::parse(items, program_is, module_name.c_str());

            auto module = std::make_unique<impala::Module>(module_name.c_str(), std::move(items));
            impala::num_warnings() = 0;
            impala::num_errors()   = 0;
            std::unique_ptr<impala::TypeTable> typetable;
            impala::check(typetable, module.get(), false);
            if (impala::num_errors() != 0)
                return -1;

            thorin::World world(module_name);
            impala::emit(world, module.get());

            world.opt();

            thorin::Backends backends(world);
            llvm_module = std::move(backends.cpu_cg->emit(opt, debug));
            llvm_context = std::move(backends.cpu_cg->context());
            std::stringstream stream;
            llvm::raw_os_ostream llvm_stream(stream);
            llvm_module->print(llvm_stream, nullptr);
            runtime().store_cache(std::string(program), stream.str(), ".llvm");

            auto emit_to_string = [&](thorin::CodeGen* cg, std::string ext) {
                if (cg) {
                    std::ostringstream stream;
                    cg->emit(stream, opt, debug);
                    runtime().store_cache(ext + std::string(program), stream.str(), ext);
                    runtime().register_file(std::string(module_name) + ext, stream.str());
                }
            };
            emit_to_string(backends.opencl_cg.get(), ".cl");
            emit_to_string(backends.cuda_cg.get(),   ".cu");
            emit_to_string(backends.nvvm_cg.get(),   ".nvvm");
            emit_to_string(backends.amdgpu_cg.get(), ".amdgpu");
            if (backends.hls_cg.get())
                error("JIT compilation of hls not supported!");
        } else {
            llvm::SMDiagnostic diagnostic_err;
            llvm_context.reset(new llvm::LLVMContext());
            llvm_module = llvm::parseIR(llvm::MemoryBuffer::getMemBuffer(cached_llvm)->getMemBufferRef(), diagnostic_err, *llvm_context);

            auto load_backend_src = [&](std::string ext) {
                std::string cached_src = runtime().load_cache(ext + std::string(program), ext);
                if (!cached_src.empty())
                    runtime().register_file(module_name + ext, cached_src);
            };
            load_backend_src(".cl");
            load_backend_src(".cu");
            load_backend_src(".nvvm");
            load_backend_src(".amdgpu");
        }

        auto engine = llvm::EngineBuilder(std::move(llvm_module))
            .setEngineKind(llvm::EngineKind::JIT)
            .setOptLevel(   opt == 0  ? llvm::CodeGenOpt::None    :
                            opt == 1  ? llvm::CodeGenOpt::Less    :
                            opt == 2  ? llvm::CodeGenOpt::Default :
                        /* opt == 3 */ llvm::CodeGenOpt::Aggressive)
            .create();
        if (!engine)
            return -1;

        engine->finalizeObject();
        programs.push_back(Program(engine));

        return (int32_t)programs.size() - 1;
    }

    void* lookup_function(int32_t key, const char* fn_name) {
        if (key == -1)
            return nullptr;

        return (void *)programs[key].engine->getFunctionAddress(fn_name);
    }

    void link(const char* lib) {
        llvm::sys::DynamicLibrary::LoadLibraryPermanently(lib);
    }
};

JIT& jit() {
    static std::unique_ptr<JIT> jit(new JIT());
    return *jit;
}

void anydsl_link(const char* lib) {
    jit().link(lib);
}

int32_t anydsl_compile(const char* program, uint32_t size, uint32_t opt) {
    return jit().compile(program, size, opt);
}

void* anydsl_lookup_function(int32_t key, const char* fn_name) {
    return jit().lookup_function(key, fn_name);
}
