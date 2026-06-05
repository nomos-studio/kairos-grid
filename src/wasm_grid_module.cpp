// SPDX-License-Identifier: GPL-3.0-or-later
//
// WasmGridModule — wasmtime-backed GridModule wrapping a Faust-compiled .wasm.
//
// Architecture:
//   WasmModuleCache — Meyers singleton; holds one wasm_engine_t and one
//     compiled wasmtime_module_t per path.  JIT compilation is deferred to the
//     first create() call for a given path; subsequent calls share the compiled
//     module and only allocate a new store+instance.
//
//   WasmDspImpl — per-instance wasmtime state: store, instance, exported
//     function handles, WASM memory handle, scratch memory layout, and the
//     Faust param metadata loaded from the companion JSON sidecar.
//
// Port layout (set up in create()):
//   inputs[0 .. n_audio_in-1]            — Faust audio inputs
//   inputs[n_audio_in .. +n_params-1]    — named param ports (control-rate CV)
//   outputs[0 .. n_audio_out-1]          — Faust audio outputs
//
// param_ports[] names are initially the raw Faust labels.  The setup_with_args
// callback in the module registry prefixes them with "<stem>/" once the wasm
// filename is known.
//
// Scratch memory layout (one WASM page grown by prepare()):
//   [base] in_ptrs[n_audio_in]   — i32 WASM addresses of input sample slots
//          out_ptrs[n_audio_out] — i32 WASM addresses of output sample slots
//          in_samples[n_audio_in]  — one float per channel
//          out_samples[n_audio_out] — one float per channel
//
// Faust WASM ABI (all functions take explicit dsp=0 as first arg):
//   init(0, sample_rate: i32)
//   compute(0, count: i32, in_ptrs: i32, out_ptrs: i32)
//   getNumInputs(0) -> i32
//   getNumOutputs(0) -> i32
//   setParamValue(0, wasm_addr: i32, value: f32)   -- optional

#include <kairos_grid/wasm_grid_module.hpp>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include <wasmtime.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace kairos_grid {

namespace {

static constexpr uint32_t k_wasm_page_size = 65536u;

// ---------------------------------------------------------------------------
// Minimal Faust JSON sidecar parser — no external dependency.
//
// Faust emits a JSON document whose "ui" array contains a tree of group and
// leaf nodes.  Leaf types: hslider, vslider, nentry, button, checkbox, knob.
// Each leaf carries "label" (string) and "index" (WASM memory address, i32).
// We extract only those two fields; everything else is skipped in one pass.
// ---------------------------------------------------------------------------

struct WasmParamInfo {
    std::string name;       // Faust label (stem prefix added later by registry)
    int32_t     wasm_addr;
};

static void skip_ws(const std::string& s, size_t& pos) noexcept {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
}

static std::string read_json_string(const std::string& s, size_t& pos) noexcept {
    if (pos >= s.size() || s[pos] != '"') return {};
    ++pos;
    std::string r;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\') { ++pos; if (pos < s.size()) r += s[pos++]; }
        else r += s[pos++];
    }
    if (pos < s.size()) ++pos;
    return r;
}

static int32_t read_json_int(const std::string& s, size_t& pos) noexcept {
    skip_ws(s, pos);
    const bool neg = (pos < s.size() && s[pos] == '-');
    if (neg) ++pos;
    int32_t n = 0;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
        n = n * 10 + (s[pos++] - '0');
    return neg ? -n : n;
}

// Forward declarations.
static void skip_json_value(const std::string& s, size_t& pos) noexcept;
static void walk_ui_array(const std::string& s, size_t& pos,
                           std::vector<WasmParamInfo>& out) noexcept;

static void skip_json_array(const std::string& s, size_t& pos) noexcept {
    if (pos >= s.size() || s[pos] != '[') return;
    ++pos;
    while (pos < s.size() && s[pos] != ']') {
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] != ']') skip_json_value(s, pos);
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    if (pos < s.size()) ++pos;
}

static void skip_json_object(const std::string& s, size_t& pos) noexcept {
    if (pos >= s.size() || s[pos] != '{') return;
    ++pos;
    while (pos < s.size() && s[pos] != '}') {
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == '"') {
            read_json_string(s, pos);
            skip_ws(s, pos);
            if (pos < s.size() && s[pos] == ':') ++pos;
            skip_ws(s, pos);
            skip_json_value(s, pos);
        } else if (pos < s.size() && s[pos] != '}') {
            ++pos;
        }
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    if (pos < s.size()) ++pos;
}

static void skip_json_value(const std::string& s, size_t& pos) noexcept {
    skip_ws(s, pos);
    if (pos >= s.size()) return;
    switch (s[pos]) {
        case '"': read_json_string(s, pos); break;
        case '{': skip_json_object(s, pos); break;
        case '[': skip_json_array(s, pos);  break;
        default:
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
                   s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' && s[pos] != '\r')
                ++pos;
            break;
    }
}

static constexpr const char* k_faust_leaf_types[] = {
    "hslider", "vslider", "nentry", "button", "checkbox", "knob"
};

static bool is_faust_leaf(const std::string& t) noexcept {
    for (const char* lt : k_faust_leaf_types)
        if (t == lt) return true;
    return false;
}

// Parse one Faust UI node object.  Collects "type", "label", "index".
// If the node has "items", saves that array position and recurses after the
// object is fully consumed (so items_start remains valid after skip_json_array).
static void walk_ui_node(const std::string& s, size_t& pos,
                          std::vector<WasmParamInfo>& out) noexcept {
    if (pos >= s.size() || s[pos] != '{') return;
    ++pos;

    std::string type_str, label_str;
    int32_t     index_val   = 0;
    size_t      items_start = std::string::npos;

    while (pos < s.size() && s[pos] != '}') {
        skip_ws(s, pos);
        if (pos >= s.size() || s[pos] == '}') break;
        if (s[pos] != '"') { ++pos; continue; }

        const std::string key = read_json_string(s, pos);
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ':') ++pos;
        skip_ws(s, pos);

        if (key == "type" && pos < s.size() && s[pos] == '"') {
            type_str = read_json_string(s, pos);
        } else if (key == "label" && pos < s.size() && s[pos] == '"') {
            label_str = read_json_string(s, pos);
        } else if (key == "index") {
            index_val = read_json_int(s, pos);
        } else if (key == "items" && pos < s.size() && s[pos] == '[') {
            items_start = pos;         // save before skipping
            skip_json_array(s, pos);
        } else {
            skip_json_value(s, pos);
        }

        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    if (pos < s.size()) ++pos; // skip '}'

    if (is_faust_leaf(type_str)) {
        if (!label_str.empty())
            out.push_back({std::move(label_str), index_val});
    } else if (items_start != std::string::npos) {
        walk_ui_array(s, items_start, out);
    }
}

static void walk_ui_array(const std::string& s, size_t& pos,
                           std::vector<WasmParamInfo>& out) noexcept {
    if (pos >= s.size() || s[pos] != '[') return;
    ++pos;
    while (pos < s.size() && s[pos] != ']') {
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == '{')
            walk_ui_node(s, pos, out);
        else if (pos < s.size() && s[pos] != ']')
            skip_json_value(s, pos);
        skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
    }
    if (pos < s.size()) ++pos;
}

// Load param metadata from <wasm_path_stem>.json.
// Returns an empty vector if the sidecar is absent or malformed.
static std::vector<WasmParamInfo> load_wasm_params(const std::string& wasm_path) noexcept {
    std::string json_path = wasm_path;
    const auto  dot       = json_path.rfind('.');
    if (dot != std::string::npos)
        json_path.replace(dot, std::string::npos, ".json");
    else
        json_path += ".json";

    std::FILE* f = std::fopen(json_path.c_str(), "r");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { std::fclose(f); return {}; }

    std::string buf(static_cast<size_t>(file_size), '\0');
    std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    // Locate the top-level "ui" array.
    const size_t ui_key = buf.find("\"ui\"");
    if (ui_key == std::string::npos) return {};
    size_t pos = ui_key + 4;
    skip_ws(buf, pos);
    if (pos >= buf.size() || buf[pos] != ':') return {};
    ++pos;
    skip_ws(buf, pos);

    std::vector<WasmParamInfo> params;
    if (pos < buf.size() && buf[pos] == '[')
        walk_ui_array(buf, pos, params);
    return params;
}

// ---------------------------------------------------------------------------
// Faust math import callbacks.
//
// Faust 2.80+ externalises standard math functions as "env._sinf" etc.
// when using os.osc(), filters, and most non-trivial DSP.  Each callback
// is a distinct static function to avoid unsafe void*↔function-pointer
// casting.  The linker resolves these at instantiation time.
// ---------------------------------------------------------------------------

#define FAUST_MATH_CB_1_1(name__, fn__)                                         \
static wasm_trap_t* name__(void*, wasmtime_caller_t*,                           \
                            const wasmtime_val_t* a, size_t,                    \
                            wasmtime_val_t* r, size_t) noexcept {               \
    r[0].kind = WASMTIME_F32; r[0].of.f32 = fn__(a[0].of.f32); return nullptr; \
}

#define FAUST_MATH_CB_2_1(name__, fn__)                                                      \
static wasm_trap_t* name__(void*, wasmtime_caller_t*,                                        \
                            const wasmtime_val_t* a, size_t,                                 \
                            wasmtime_val_t* r, size_t) noexcept {                            \
    r[0].kind = WASMTIME_F32; r[0].of.f32 = fn__(a[0].of.f32, a[1].of.f32); return nullptr; \
}

FAUST_MATH_CB_1_1(cb_sinf,   std::sinf)
FAUST_MATH_CB_1_1(cb_cosf,   std::cosf)
FAUST_MATH_CB_1_1(cb_tanf,   std::tanf)
FAUST_MATH_CB_1_1(cb_expf,   std::expf)
FAUST_MATH_CB_1_1(cb_logf,   std::logf)
FAUST_MATH_CB_1_1(cb_log10f, std::log10f)
FAUST_MATH_CB_1_1(cb_sqrtf,  std::sqrtf)
FAUST_MATH_CB_1_1(cb_fabsf,  std::fabsf)
FAUST_MATH_CB_1_1(cb_floorf, std::floorf)
FAUST_MATH_CB_1_1(cb_ceilf,  std::ceilf)
FAUST_MATH_CB_1_1(cb_roundf, std::roundf)
FAUST_MATH_CB_1_1(cb_asinf,  std::asinf)
FAUST_MATH_CB_1_1(cb_acosf,  std::acosf)
FAUST_MATH_CB_1_1(cb_atanf,  std::atanf)
FAUST_MATH_CB_2_1(cb_powf,   std::powf)
FAUST_MATH_CB_2_1(cb_fmodf,  std::fmodf)
FAUST_MATH_CB_2_1(cb_atan2f, std::atan2f)
FAUST_MATH_CB_2_1(cb_fminf,  std::fminf)
FAUST_MATH_CB_2_1(cb_fmaxf,  std::fmaxf)

#undef FAUST_MATH_CB_1_1
#undef FAUST_MATH_CB_2_1

// Register one f32→f32 function using the header's inline helper, which uses
// wasm_valtype_vec_new (heap-allocated) and correct ownership semantics.
static void linker_def_f32_f32(wasmtime_linker_t* linker,
                                const char* name,
                                wasmtime_func_callback_t cb) noexcept {
    wasm_functype_t* ft = wasm_functype_new_1_1(wasm_valtype_new_f32(),
                                                 wasm_valtype_new_f32());
    wasmtime_error_t* err =
        wasmtime_linker_define_func(linker, "env", 3, name, std::strlen(name),
                                     ft, cb, nullptr, nullptr);
    wasm_functype_delete(ft);
    if (err) wasmtime_error_delete(err);
}

// Register one (f32,f32)→f32 function.
static void linker_def_f32_f32_f32(wasmtime_linker_t* linker,
                                    const char* name,
                                    wasmtime_func_callback_t cb) noexcept {
    wasm_functype_t* ft = wasm_functype_new_2_1(wasm_valtype_new_f32(),
                                                 wasm_valtype_new_f32(),
                                                 wasm_valtype_new_f32());
    wasmtime_error_t* err =
        wasmtime_linker_define_func(linker, "env", 3, name, std::strlen(name),
                                     ft, cb, nullptr, nullptr);
    wasm_functype_delete(ft);
    if (err) wasmtime_error_delete(err);
}

// ---------------------------------------------------------------------------
// Module cache — one wasm_engine_t + one compiled module per wasm_path.
// Singleton lifetime matches the plugin process.
//
// The linker is pre-populated with the "env.*" math functions that Faust
// 2.80+ externalises (env._sinf, env._cosf, etc.).  Any Faust-compiled WASM
// that imports these resolves them at instantiation time; modules with no
// imports (e.g. phasor-only DSP) instantiate normally via the same linker.
// ---------------------------------------------------------------------------

struct WasmModuleCache {
    wasm_engine_t*                                       engine;
    wasmtime_linker_t*                                   linker;
    std::mutex                                           mtx;
    std::unordered_map<std::string, wasmtime_module_t*> modules;

    static WasmModuleCache& get() noexcept {
        static WasmModuleCache inst;
        return inst;
    }

    wasmtime_module_t* get_or_compile(const std::string& path,
                                       const uint8_t* bytes, size_t nbytes) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = modules.find(path);
        if (it != modules.end()) return it->second;
        wasmtime_module_t* mod = nullptr;
        wasmtime_error_t*  err = wasmtime_module_new(engine, bytes, nbytes, &mod);
        if (err) { wasmtime_error_delete(err); return nullptr; }
        modules.emplace(path, mod);
        return mod;
    }

  private:
    WasmModuleCache() : engine(wasm_engine_new()), linker(wasmtime_linker_new(engine)) {
        linker_def_f32_f32    (linker, "_sinf",   cb_sinf);
        linker_def_f32_f32    (linker, "_cosf",   cb_cosf);
        linker_def_f32_f32    (linker, "_tanf",   cb_tanf);
        linker_def_f32_f32    (linker, "_expf",   cb_expf);
        linker_def_f32_f32    (linker, "_logf",   cb_logf);
        linker_def_f32_f32    (linker, "_log10f", cb_log10f);
        linker_def_f32_f32    (linker, "_sqrtf",  cb_sqrtf);
        linker_def_f32_f32    (linker, "_fabsf",  cb_fabsf);
        linker_def_f32_f32    (linker, "_floorf", cb_floorf);
        linker_def_f32_f32    (linker, "_ceilf",  cb_ceilf);
        linker_def_f32_f32    (linker, "_roundf", cb_roundf);
        linker_def_f32_f32    (linker, "_asinf",  cb_asinf);
        linker_def_f32_f32    (linker, "_acosf",  cb_acosf);
        linker_def_f32_f32    (linker, "_atanf",  cb_atanf);
        linker_def_f32_f32_f32(linker, "_powf",   cb_powf);
        linker_def_f32_f32_f32(linker, "_fmodf",  cb_fmodf);
        linker_def_f32_f32_f32(linker, "_atan2f", cb_atan2f);
        linker_def_f32_f32_f32(linker, "_fminf",  cb_fminf);
        linker_def_f32_f32_f32(linker, "_fmaxf",  cb_fmaxf);
    }
    ~WasmModuleCache() {
        for (auto& [p, m] : modules) wasmtime_module_delete(m);
        wasmtime_linker_delete(linker);
        wasm_engine_delete(engine);
    }
};

// ---------------------------------------------------------------------------
// wasmtime call helpers (identical in spirit to wasm_bridge_plugin.cpp)
// ---------------------------------------------------------------------------

static inline wasmtime_val_t wt_i32(int32_t v) noexcept {
    wasmtime_val_t val{};
    val.kind   = WASMTIME_I32;
    val.of.i32 = v;
    return val;
}

static inline wasmtime_val_t wt_f32(float v) noexcept {
    wasmtime_val_t val{};
    val.kind   = WASMTIME_F32;
    val.of.f32 = v;
    return val;
}

static void call_wasm(wasmtime_context_t* ctx, const wasmtime_func_t* fn,
                      const wasmtime_val_t* args, size_t nargs,
                      wasmtime_val_t* results, size_t nresults) noexcept {
    wasm_trap_t*      trap = nullptr;
    wasmtime_error_t* err  =
        wasmtime_func_call(ctx, fn, args, nargs, results, nresults, &trap);
    if (err)  wasmtime_error_delete(err);
    if (trap) wasm_trap_delete(trap);
}

static bool get_export_func(wasmtime_context_t* ctx, const wasmtime_instance_t* inst,
                             const char* name, wasmtime_func_t* out) noexcept {
    wasmtime_extern_t ext{};
    if (!wasmtime_instance_export_get(ctx, inst, name, std::strlen(name), &ext))
        return false;
    if (ext.kind != WASMTIME_EXTERN_FUNC) return false;
    *out = ext.of.func;
    return true;
}

static bool get_export_memory(wasmtime_context_t* ctx, const wasmtime_instance_t* inst,
                               const char* name, wasmtime_memory_t* out) noexcept {
    wasmtime_extern_t ext{};
    if (!wasmtime_instance_export_get(ctx, inst, name, std::strlen(name), &ext))
        return false;
    if (ext.kind != WASMTIME_EXTERN_MEMORY) return false;
    *out = ext.of.memory;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// WasmDspImpl — fully defined here (incomplete type in the header).
// ---------------------------------------------------------------------------

struct WasmDspImpl {
    wasmtime_store_t*   store{nullptr};
    wasmtime_instance_t instance{};
    wasmtime_memory_t   wasm_memory{};
    wasmtime_func_t     fn_init{};
    wasmtime_func_t     fn_compute{};
    wasmtime_func_t     fn_set_param_value{};
    bool                has_set_param{false};

    int32_t num_audio_in{0};
    int32_t num_audio_out{0};

    // Scratch layout addresses within WASM linear memory.
    // Populated by the first prepare() call; stable for the module's lifetime.
    int32_t wasm_in_ptrs{0};
    int32_t wasm_out_ptrs{0};
    int32_t wasm_in_samples{0};
    int32_t wasm_out_samples{0};
    bool    scratch_ready{false};

    std::vector<WasmParamInfo> params;

    ~WasmDspImpl() {
        if (store) wasmtime_store_delete(store);
    }
};

// ---------------------------------------------------------------------------
// WasmGridModule implementation
// ---------------------------------------------------------------------------

// static factory
std::unique_ptr<WasmGridModule> WasmGridModule::create(const std::string& wasm_path) {
    if (wasm_path.empty()) return nullptr;

    // Read .wasm bytes.
    std::FILE* f = std::fopen(wasm_path.c_str(), "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (file_size <= 0) { std::fclose(f); return nullptr; }
    std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
    std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);

    // Compile or retrieve from cache.
    WasmModuleCache&   cache  = WasmModuleCache::get();
    wasmtime_module_t* module = cache.get_or_compile(wasm_path, bytes.data(), bytes.size());
    if (!module) return nullptr;

    // Create per-instance store + instance.
    auto impl = std::make_unique<WasmDspImpl>();
    impl->store = wasmtime_store_new(cache.engine, nullptr, nullptr);
    if (!impl->store) return nullptr;

    wasmtime_context_t* ctx  = wasmtime_store_context(impl->store);
    wasm_trap_t*        trap = nullptr;
    wasmtime_error_t*   err  =
        wasmtime_linker_instantiate(cache.linker, ctx, module, &impl->instance, &trap);
    if (err)  { wasmtime_error_delete(err);  return nullptr; }
    if (trap) { wasm_trap_delete(trap);      return nullptr; }

    // Cache exported function handles.
    wasmtime_func_t fn_get_num_inputs{}, fn_get_num_outputs{};
    if (!get_export_func(ctx, &impl->instance, "init",          &impl->fn_init)         ||
        !get_export_func(ctx, &impl->instance, "compute",       &impl->fn_compute)       ||
        !get_export_func(ctx, &impl->instance, "getNumInputs",  &fn_get_num_inputs)      ||
        !get_export_func(ctx, &impl->instance, "getNumOutputs", &fn_get_num_outputs)     ||
        !get_export_memory(ctx, &impl->instance, "memory",      &impl->wasm_memory)) {
        return nullptr;
    }
    impl->has_set_param =
        get_export_func(ctx, &impl->instance, "setParamValue", &impl->fn_set_param_value);

    // Query Faust audio I/O counts.
    wasmtime_val_t dsp_arg = wt_i32(0);
    wasmtime_val_t result{};
    call_wasm(ctx, &fn_get_num_inputs,  &dsp_arg, 1, &result, 1);
    impl->num_audio_in  = result.of.i32;
    call_wasm(ctx, &fn_get_num_outputs, &dsp_arg, 1, &result, 1);
    impl->num_audio_out = result.of.i32;

    // Load companion JSON param metadata.
    impl->params = load_wasm_params(wasm_path);

    const int n_audio_in  = impl->num_audio_in;
    const int n_audio_out = impl->num_audio_out;
    const int n_params    = static_cast<int>(impl->params.size());

    // Construct module (private ctor).
    auto* wm = new (std::nothrow)
        WasmGridModule(std::move(impl), n_audio_in, n_audio_out, n_params);
    if (!wm) return nullptr;

    // Populate param_ports with raw labels (stem prefix applied later by registry).
    for (int i = 0; i < n_params; ++i) {
        wm->param_ports.push_back({
            wm->impl_->params[static_cast<size_t>(i)].name,
            n_audio_in + i
        });
    }

    return std::unique_ptr<WasmGridModule>(wm);
}

WasmGridModule::WasmGridModule(std::unique_ptr<WasmDspImpl> impl,
                                int n_audio_in, int n_audio_out, int n_params)
    : GridModule(n_audio_in + n_params, n_audio_out),
      impl_(std::move(impl)),
      n_audio_in_(n_audio_in),
      n_audio_out_(n_audio_out),
      n_params_(n_params),
      last_param_voltages_(static_cast<size_t>(n_params),
                            std::numeric_limits<float>::quiet_NaN()) {}

// Destructor must be defined here where WasmDspImpl is complete.
WasmGridModule::~WasmGridModule() = default;

void WasmGridModule::prepare(const GridProcessArgs& args) {
    if (!impl_) return;
    wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);

    // Faust init(dsp=0, sample_rate).
    wasmtime_val_t init_args[2] = {
        wt_i32(0),
        wt_i32(static_cast<int32_t>(args.sample_rate))
    };
    call_wasm(ctx, &impl_->fn_init, init_args, 2, nullptr, 0);

    if (!impl_->scratch_ready) {
        // Grow WASM linear memory by one page; record the base of that new page.
        uint64_t          prev_pages = 0;
        wasmtime_error_t* err =
            wasmtime_memory_grow(ctx, &impl_->wasm_memory, 1, &prev_pages);
        if (err) { wasmtime_error_delete(err); return; }

        const int32_t base = static_cast<int32_t>(prev_pages * k_wasm_page_size);
        impl_->wasm_in_ptrs    = base;
        impl_->wasm_out_ptrs   = base + impl_->num_audio_in * 4;
        impl_->wasm_in_samples = impl_->wasm_out_ptrs  + impl_->num_audio_out * 4;
        impl_->wasm_out_samples = impl_->wasm_in_samples + impl_->num_audio_in * 4;
        impl_->scratch_ready   = true;
    }

    // Write channel pointer arrays (count=1 → one float slot per channel).
    uint8_t* mem = wasmtime_memory_data(ctx, &impl_->wasm_memory);
    for (int32_t c = 0; c < impl_->num_audio_in; ++c) {
        int32_t addr = impl_->wasm_in_samples + c * 4;
        std::memcpy(mem + impl_->wasm_in_ptrs + c * 4, &addr, 4);
    }
    for (int32_t c = 0; c < impl_->num_audio_out; ++c) {
        int32_t addr = impl_->wasm_out_samples + c * 4;
        std::memcpy(mem + impl_->wasm_out_ptrs + c * 4, &addr, 4);
    }

    // Force all params to be re-pushed on the next process() call.
    std::fill(last_param_voltages_.begin(), last_param_voltages_.end(),
              std::numeric_limits<float>::quiet_NaN());
}

void WasmGridModule::process(const GridProcessArgs& /*args*/) {
    if (!impl_ || !impl_->scratch_ready) return;

    wasmtime_context_t* ctx = wasmtime_store_context(impl_->store);
    uint8_t* mem = wasmtime_memory_data(ctx, &impl_->wasm_memory);

    // Copy audio inputs into WASM scratch (one float per channel).
    for (int c = 0; c < n_audio_in_; ++c) {
        const float v = inputs[static_cast<size_t>(c)].voltage;
        std::memcpy(mem + impl_->wasm_in_samples + c * 4, &v, 4);
    }

    // Dirty-tracked param updates via setParamValue.
    // NaN initial values ensure the first call always pushes every param.
    if (impl_->has_set_param) {
        for (int i = 0; i < n_params_; ++i) {
            const float v = inputs[static_cast<size_t>(n_audio_in_ + i)].voltage;
            if (v != last_param_voltages_[static_cast<size_t>(i)]) {
                last_param_voltages_[static_cast<size_t>(i)] = v;
                wasmtime_val_t set_args[3] = {
                    wt_i32(0),
                    wt_i32(impl_->params[static_cast<size_t>(i)].wasm_addr),
                    wt_f32(v),
                };
                call_wasm(ctx, &impl_->fn_set_param_value, set_args, 3, nullptr, 0);
            }
        }
    }

    // compute(dsp=0, count=1, in_ptrs, out_ptrs).
    wasmtime_val_t compute_args[4] = {
        wt_i32(0),
        wt_i32(1),
        wt_i32(impl_->wasm_in_ptrs),
        wt_i32(impl_->wasm_out_ptrs),
    };
    call_wasm(ctx, &impl_->fn_compute, compute_args, 4, nullptr, 0);

    // Copy audio outputs from WASM scratch.
    for (int c = 0; c < n_audio_out_; ++c) {
        float v = 0.f;
        std::memcpy(&v, mem + impl_->wasm_out_samples + c * 4, 4);
        outputs[static_cast<size_t>(c)].voltage = v;
    }
}

} // namespace kairos_grid
