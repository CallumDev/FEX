#include "analysis.h"
#include "interface.h"

#include <clang/Frontend/CompilerInstance.h>

#include <fstream>
#include <numeric>
#include <iostream>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <openssl/sha.h>

class GenerateThunkLibsAction : public AnalysisAction {
public:
    GenerateThunkLibsAction(const std::string& libname, const OutputFilenames&);

private:
    // Generate helper code for thunk libraries and write them to the output file
    void EmitOutput(clang::ASTContext&) override;

    const std::string& libfilename;
    std::string libname; // sanitized filename, usable as part of emitted function names
    const OutputFilenames& output_filenames;
};

GenerateThunkLibsAction::GenerateThunkLibsAction(const std::string& libname_, const OutputFilenames& output_filenames_)
    : libfilename(libname_), libname(libname_), output_filenames(output_filenames_) {
    for (auto& c : libname) {
        if (c == '-') {
            c = '_';
        }
    }
}

template<typename Fn>
static std::string format_function_args(const FunctionParams& params, Fn&& format_arg) {
    std::string ret;
    for (std::size_t idx = 0; idx < params.param_types.size(); ++idx) {
        ret += std::forward<Fn>(format_arg)(idx) + ", ";
    }
    // drop trailing ", "
    ret.resize(ret.size() > 2 ? ret.size() - 2 : 0);
    return ret;
};

void GenerateThunkLibsAction::EmitOutput(clang::ASTContext& context) {
    static auto format_decl = [](clang::QualType type, const std::string_view& name) {
        clang::QualType innermostPointee = type;
        while (innermostPointee->isPointerType()) {
          innermostPointee = innermostPointee->getPointeeType();
        }
        if (innermostPointee->isFunctionType()) {
            // Function pointer declarations (e.g. void (**callback)()) require
            // the variable name to be prefixed *and* suffixed.

            auto signature = type.getAsString();

            // Search for strings like (*), (**), or (*****). Insert the
            // variable name before the closing parenthesis
            auto needle = signature.begin();
            for (; needle != signature.end(); ++needle) {
                if (signature.end() - needle < 3 ||
                    std::string_view { &*needle, 2 } != "(*") {
                    continue;
                }
                while (*++needle == '*') {
                }
                if (*needle == ')') {
                    break;
                }
            }
            if (needle == signature.end()) {
                // It's *probably* a typedef, so this should be safe after all
                return fmt::format("{} {}", signature, name);
            } else {
                signature.insert(needle, name.begin(), name.end());
                return signature;
            }
        } else {
            return type.getAsString() + " " + std::string(name);
        }
    };

    auto format_struct_members = [](const FunctionParams& params, const char* indent) {
        std::string ret;
        for (std::size_t idx = 0; idx < params.param_types.size(); ++idx) {
            ret += indent + format_decl(params.param_types[idx].getUnqualifiedType(), fmt::format("a_{}", idx)) + ";\n";
        }
        return ret;
    };

    auto format_function_params = [](const FunctionParams& params) {
        std::string ret;
        for (std::size_t idx = 0; idx < params.param_types.size(); ++idx) {
            auto& type = params.param_types[idx];
            ret += format_decl(type, fmt::format("a_{}", idx)) + ", ";
        }
        // drop trailing ", "
        ret.resize(ret.size() > 2 ? ret.size() - 2 : 0);
        return ret;
    };

    auto get_sha256 = [this](const std::string& function_name) {
        std::string sha256_message = libname + ":" + function_name;
        std::vector<unsigned char> sha256(SHA256_DIGEST_LENGTH);
        SHA256(reinterpret_cast<const unsigned char*>(sha256_message.data()),
               sha256_message.size(),
               sha256.data());
        return sha256;
    };

    auto get_callback_name = [](std::string_view function_name, unsigned param_index) -> std::string {
        return fmt::format("{}CBFN{}", function_name, param_index);
    };

    // Files used guest-side
    if (!output_filenames.guest.empty()) {
        std::ofstream file(output_filenames.guest);

        // Guest->Host transition points for API functions
        file << "extern \"C\" {\n";
        for (auto& thunk : thunks) {
            const auto& function_name = thunk.function_name;
            auto sha256 = get_sha256(function_name);
            fmt::print( file, "MAKE_THUNK({}, {}, \"{:#02x}\")\n",
                        libname, function_name, fmt::join(sha256, ", "));
        }
        file << "}\n";

        // Guest->Host transition points for invoking runtime host-function pointers based on their signature
        for (auto type_it = funcptr_types.begin(); type_it != funcptr_types.end(); ++type_it) {
            auto* type = *type_it;
            std::string funcptr_signature = clang::QualType { type, 0 }.getAsString();

            auto cb_sha256 = get_sha256("fexcallback_" + funcptr_signature);

            // Thunk used for guest-side calls to host function pointers
            file << "  // " << funcptr_signature << "\n";
            auto funcptr_idx = std::distance(funcptr_types.begin(), type_it);
            fmt::print( file, "  MAKE_CALLBACK_THUNK(callback_{}, {}, \"{:#02x}\");\n",
                        funcptr_idx, funcptr_signature, fmt::join(cb_sha256, ", "));
        }

        // Thunks-internal packing functions
        file << "extern \"C\" {\n";
        for (auto& data : thunks) {
            const auto& function_name = data.function_name;
            bool is_void = data.return_type->isVoidType();
            file << "FEX_PACKFN_LINKAGE auto fexfn_pack_" << function_name << "(";
            for (std::size_t idx = 0; idx < data.param_types.size(); ++idx) {
                auto& type = data.param_types[idx];
                file << (idx == 0 ? "" : ", ") << format_decl(type, fmt::format("a_{}", idx));
            }
            // Using trailing return type as it makes handling function pointer returns much easier
            file << ") -> " << data.return_type.getAsString() << " {\n";
            file << "  struct {\n";
            for (std::size_t idx = 0; idx < data.param_types.size(); ++idx) {
                auto& type = data.param_types[idx];
                file << "    " << format_decl(type.getUnqualifiedType(), fmt::format("a_{}", idx)) << ";\n";
            }
            if (!is_void) {
                file << "    " << format_decl(data.return_type, "rv") << ";\n";
            } else if (data.param_types.size() == 0) {
                // Avoid "empty struct has size 0 in C, size 1 in C++" warning
                file << "    char force_nonempty;\n";
            }
            file << "  } args;\n";

            for (std::size_t idx = 0; idx < data.param_types.size(); ++idx) {
                auto cb = data.callbacks.find(idx);

                file << "  args.a_" << idx << " = ";
                if (cb == data.callbacks.end() || cb->second.is_stub || cb->second.is_guest) {
                    file << "a_" << idx << ";\n";
                } else {
                    // Before passing guest function pointers to the host, wrap them in a host-callable trampoline
                    fmt::print(file, "AllocateHostTrampolineForGuestFunction(a_{});\n", idx);
                }
            }
            file << "  fexthunks_" << libname << "_" << function_name << "(&args);\n";
            if (!is_void) {
                file << "  return args.rv;\n";
            }
            file << "}\n";
        }
        file << "}\n";

        // Publicly exports equivalent to symbols exported from the native guest library
        file << "extern \"C\" {\n";
        for (auto& data : thunked_api) {
            if (data.custom_guest_impl) {
                continue;
            }

            const auto& function_name = data.function_name;

            file << "__attribute__((alias(\"fexfn_pack_" << function_name << "\"))) auto " << function_name << "(";
            for (std::size_t idx = 0; idx < data.param_types.size(); ++idx) {
                auto& type = data.param_types[idx];
                file << (idx == 0 ? "" : ", ") << format_decl(type, "a_" + std::to_string(idx));
            }
            file << ") -> " << data.return_type.getAsString() << ";\n";
        }
        file << "}\n";

        // Symbol enumerators
        for (std::size_t namespace_idx = 0; namespace_idx < namespaces.size(); ++namespace_idx) {
            const auto& ns = namespaces[namespace_idx];
            file << "#define FOREACH_" << ns.name << (ns.name.empty() ? "" : "_") << "SYMBOL(EXPAND) \\\n";
            for (auto& symbol : thunked_api) {
                if (symbol.symtable_namespace.value_or(0) == namespace_idx) {
                    file << "  EXPAND(" << symbol.function_name << ", \"TODO\") \\\n";
                }
            }
            file << "\n";
        }
    }

    // Files used host-side
    if (!output_filenames.host.empty()) {
        std::ofstream file(output_filenames.host);

        // Forward declarations for symbols loaded from the native host library
        for (auto& import : thunked_api) {
            const auto& function_name = import.function_name;
            const char* variadic_ellipsis = import.is_variadic ? ", ..." : "";
            file << "using fexldr_type_" << libname << "_" << function_name << " = auto " << "(" << format_function_params(import) << variadic_ellipsis << ") -> " << import.return_type.getAsString() << ";\n";
            file << "static fexldr_type_" << libname << "_" << function_name << " *fexldr_ptr_" << libname << "_" << function_name << ";\n";
        }

        file << "extern \"C\" {\n";
        for (auto& thunk : thunks) {
            const auto& function_name = thunk.function_name;

            // Generate stub callbacks
            for (auto& [cb_idx, cb] : thunk.callbacks) {
                if (cb.is_stub) {
                    const char* variadic_ellipsis = cb.is_variadic ? ", ..." : "";
                    auto cb_function_name = "fexfn_unpack_" + get_callback_name(function_name, cb_idx) + "_stub";
                    file << "[[noreturn]] static " << cb.return_type.getAsString() << " "
                         << cb_function_name << "("
                         << format_function_params(cb) << variadic_ellipsis << ") {\n";
                    file << "  fprintf(stderr, \"FATAL: Attempted to invoke callback stub for " << function_name << "\\n\");\n";
                    file << "  std::abort();\n";
                    file << "}\n";
                }
            }

            // Forward declarations for user-provided implementations
            if (thunk.custom_host_impl) {
                file << "static auto fexfn_impl_" << libname << "_" << function_name << "(";
                for (std::size_t idx = 0; idx < thunk.param_types.size(); ++idx) {
                    // TODO: fex_guest_function_ptr for guest callbacks?
                    auto& type = thunk.param_types[idx];

                    file << (idx == 0 ? "" : ", ");

                    auto cb = thunk.callbacks.find(idx);
                    if (cb != thunk.callbacks.end() && cb->second.is_guest) {
                        file << "fex_guest_function_ptr a_" << idx;
                    } else {
                      file << format_decl(type, fmt::format("a_{}", idx));
                    }
                }
                // Using trailing return type as it makes handling function pointer returns much easier
                file << ") -> " << thunk.return_type.getAsString() << ";\n";
            }

            // Packed argument structs used in fexfn_unpack_*
            auto GeneratePackedArgs = [&](const auto &function_name, const auto &thunk) -> std::string {
                std::string struct_name = "fexfn_packed_args_" + libname + "_" + function_name;
                file << "struct " << struct_name << " {\n";

                file << format_struct_members(thunk, "  ");
                if (!thunk.return_type->isVoidType()) {
                    file << "  " << format_decl(thunk.return_type, "rv") << ";\n";
                } else if (thunk.param_types.size() == 0) {
                    // Avoid "empty struct has size 0 in C, size 1 in C++" warning
                    file << "    char force_nonempty;\n";
                }
                file << "};\n";
                return struct_name;
            };
            auto struct_name = GeneratePackedArgs(function_name, thunk);

            // Unpacking functions
            auto function_to_call = "fexldr_ptr_" + libname + "_" + function_name;
            if (thunk.custom_host_impl) {
                function_to_call = "fexfn_impl_" + libname + "_" + function_name;
            }

            file << "static void fexfn_unpack_" << libname << "_" << function_name << "(" << struct_name << "* args) {\n";
            file << (thunk.return_type->isVoidType() ? "  " : "  args->rv = ") << function_to_call << "(";
            {
                auto format_param = [&](std::size_t idx) {
                    auto cb = thunk.callbacks.find(idx);
                    if (cb != thunk.callbacks.end() && cb->second.is_stub) {
                        return "fexfn_unpack_" + get_callback_name(function_name, cb->first) + "_stub";
                    } else if (cb != thunk.callbacks.end() && cb->second.is_guest) {
                        return fmt::format("fex_guest_function_ptr {{ args->a_{} }}", idx);
                    } else if (cb != thunk.callbacks.end()) {
                        auto arg_name = fmt::format("args->a_{}", idx);
                        // Use comma operator to inject a function call before returning the argument
                        return "(FinalizeHostTrampolineForGuestFunction(" + arg_name + "), " + arg_name + ")";

                    } else {
                        return fmt::format("args->a_{}", idx);
                    }
                };

                file << format_function_args(thunk, format_param);
            }
            file << ");\n";
            file << "}\n";
        }
        file << "}\n";

        // Endpoints for Guest->Host invocation of API functions
        file << "static ExportEntry exports[] = {\n";
        for (auto& thunk : thunks) {
            const auto& function_name = thunk.function_name;
            auto sha256 = get_sha256(function_name);
            fmt::print( file, "  {{(uint8_t*)\"\\x{:02x}\", (void(*)(void *))&fexfn_unpack_{}_{}}}, // {}:{}\n",
                        fmt::join(sha256, "\\x"), libname, function_name, libname, function_name);
        }

        // Endpoints for Guest->Host invocation of runtime host-function pointers
        for (auto& type : funcptr_types) {
            std::string mangled_name = clang::QualType { type, 0 }.getAsString();
            auto cb_sha256 = get_sha256("fexcallback_" + mangled_name);
            fmt::print( file, "  {{(uint8_t*)\"\\x{:02x}\", (void(*)(void *))&CallbackUnpack<{}>::ForIndirectCall}},\n",
                        fmt::join(cb_sha256, "\\x"), mangled_name);
        }
        file << "  { nullptr, nullptr }\n";
        file << "};\n";

        // Symbol lookup from native host library
        file << "static void* fexldr_ptr_" << libname << "_so;\n";
        file << "extern \"C\" bool fexldr_init_" << libname << "() {\n";

        std::string version_suffix;
        if (lib_version) {
          version_suffix = '.' + std::to_string(*lib_version);
        }
        const std::string library_filename = libfilename + ".so" + version_suffix;

        // Load the host library in the global symbol namespace.
        // This follows how these libraries get loaded in a non-emulated environment,
        // Either by directly linking to the library or a loader (In OpenGL or Vulkan) putting everything in the global namespace.
        file << "  fexldr_ptr_" << libname << "_so = dlopen(\"" << library_filename << "\", RTLD_GLOBAL | RTLD_LAZY);\n";

        file << "  if (!fexldr_ptr_" << libname << "_so) { return false; }\n\n";
        for (auto& import : thunked_api) {
            fmt::print( file, "  (void*&)fexldr_ptr_{}_{} = {}(fexldr_ptr_{}_so, \"{}\");\n",
                        libname, import.function_name, import.host_loader, libname, import.function_name);
        }
        file << "  return true;\n";
        file << "}\n";
    }
}

bool GenerateThunkLibsActionFactory::runInvocation(
    std::shared_ptr<clang::CompilerInvocation> Invocation, clang::FileManager *Files,
    std::shared_ptr<clang::PCHContainerOperations> PCHContainerOps,
    clang::DiagnosticConsumer *DiagConsumer) {
  clang::CompilerInstance Compiler(std::move(PCHContainerOps));
  Compiler.setInvocation(std::move(Invocation));
  Compiler.setFileManager(Files);

  GenerateThunkLibsAction Action(libname, output_filenames);

  Compiler.createDiagnostics(DiagConsumer, false);
  if (!Compiler.hasDiagnostics())
    return false;

  Compiler.createSourceManager(*Files);

  const bool Success = Compiler.ExecuteAction(Action);

  Files->clearStatCache();
  return Success;
}
