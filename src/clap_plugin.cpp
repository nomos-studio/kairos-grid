// SPDX-License-Identifier: GPL-3.0-or-later
// CLAP entry point for kairos-grid — wraps GridEngine behind the CLAP ABI.
//
// Graph: EnvironmentModule + AudioInputModule + AudioOutputModule.
//   - AudioInput/Output bridge CLAP audio buffers to per-sample grid I/O.
//   - Param bus (nomos-rt → grid) exposed as CLAP automatable parameters.
//   - Transport and note events drive EnvironmentModule via the param frame.

#include <kairos_grid/audio_modules.hpp>
#include <kairos_grid/environment_module.hpp>
#include <kairos_grid/grid_graph.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>   // memcpy in stream helpers
#include <optional>
#include <vector>

namespace kairos_grid {

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------

static const char* k_features[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    nullptr,
};

static const clap_plugin_descriptor_t k_descriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id           = "studio.nomos.kairos-grid",
    .name         = "kairos-grid",
    .vendor       = "nomos-studio",
    .url          = "",
    .manual_url   = "",
    .support_url  = "",
    .version      = "0.1.0",
    .description  = "Bitwig Grid-style modular engine with nomos-rt control plane",
    .features     = k_features,
};

// ---------------------------------------------------------------------------
// KairosGridPlugin — one instance per plugin instantiation
// ---------------------------------------------------------------------------

class KairosGridPlugin {
  public:
    explicit KairosGridPlugin(const clap_host_t* host) : host_(host) {
        clap_plugin_.desc             = &k_descriptor;
        clap_plugin_.plugin_data      = this;
        clap_plugin_.init             = &s_init;
        clap_plugin_.destroy          = &s_destroy;
        clap_plugin_.activate         = &s_activate;
        clap_plugin_.deactivate       = &s_deactivate;
        clap_plugin_.start_processing = &s_start_processing;
        clap_plugin_.stop_processing  = &s_stop_processing;
        clap_plugin_.reset            = &s_reset;
        clap_plugin_.process          = &s_process;
        clap_plugin_.get_extension    = &s_get_extension;
        clap_plugin_.on_main_thread   = &s_on_main_thread;
    }

    const clap_plugin_t* clap_plugin() const noexcept { return &clap_plugin_; }

  private:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    bool init() {
        build_engine(48000.f);
        return engine_.has_value();
    }

    void destroy() { delete this; }

    bool activate(double sample_rate, uint32_t /*min_frames*/, uint32_t /*max_frames*/) {
        sample_rate_ = static_cast<float>(sample_rate);
        if (!engine_.has_value())
            build_engine(sample_rate_);
        else
            engine_->prepare(sample_rate_);
        // Preserve any state loaded before activate() (e.g. on project open).
        // Only resize — and zero — if the schema size changed.
        const auto new_size = static_cast<std::size_t>(engine_->port_schema().size());
        if (param_frame_.size() != new_size)
            param_frame_.assign(new_size, 0.f);
        cache_port_ids();
        return true;
    }

    void deactivate() {}

    bool start_processing() { return true; }
    void stop_processing()  {}

    void reset() {
        if (engine_.has_value())
            engine_->prepare(sample_rate_);
        param_frame_.assign(static_cast<std::size_t>(
            engine_.has_value() ? engine_->port_schema().size() : 0), 0.f);
    }

    // -----------------------------------------------------------------------
    // Audio + event processing
    // -----------------------------------------------------------------------

    clap_process_status process(const clap_process_t* proc) {
        if (!engine_.has_value()) return CLAP_PROCESS_ERROR;

        if (proc->transport)
            apply_transport(proc->transport);

        const uint32_t n_events = proc->in_events->size(proc->in_events);
        for (uint32_t i = 0; i < n_events; ++i) {
            const clap_event_header_t* hdr = proc->in_events->get(proc->in_events, i);
            if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            dispatch_event(hdr);
        }

        // Route CLAP audio buffers into the grid before processing.
        if (audio_in_ && proc->audio_inputs_count > 0)
            audio_in_->set_buffers(proc->audio_inputs[0].data32,
                                   proc->audio_inputs[0].channel_count,
                                   proc->frames_count);
        if (audio_out_ && proc->audio_outputs_count > 0)
            audio_out_->set_buffers(proc->audio_outputs[0].data32,
                                    proc->audio_outputs[0].channel_count,
                                    proc->frames_count);

        engine_->apply_params(param_frame_);
        engine_->step_block(static_cast<int>(proc->frames_count));

        return CLAP_PROCESS_CONTINUE;
    }

    // -----------------------------------------------------------------------
    // Extensions
    // -----------------------------------------------------------------------

    const void* get_extension(const char* id) const {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &s_audio_ports_ext;
        if (std::strcmp(id, CLAP_EXT_PARAMS)      == 0) return &s_params_ext;
        if (std::strcmp(id, CLAP_EXT_STATE)        == 0) return &s_state_ext;
        return nullptr;
    }

    void on_main_thread() {}

    // -----------------------------------------------------------------------
    // Audio ports extension
    // -----------------------------------------------------------------------

    static uint32_t audio_ports_count(const clap_plugin_t* /*p*/, bool /*is_input*/) {
        return 1;
    }

    static bool audio_ports_get(const clap_plugin_t* /*p*/, uint32_t index, bool is_input,
                                 clap_audio_port_info_t* info) {
        if (index != 0) return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      is_input ? "Stereo In" : "Stereo Out");
        return true;
    }

    // -----------------------------------------------------------------------
    // Params extension
    // -----------------------------------------------------------------------

    static uint32_t params_count(const clap_plugin_t* p) {
        const auto* self = cast(p);
        if (!self->engine_.has_value()) return 0;
        return static_cast<uint32_t>(self->engine_->port_schema().size());
    }

    static bool params_get_info(const clap_plugin_t* p, uint32_t index,
                                 clap_param_info_t* info) {
        const auto* self = cast(p);
        if (!self->engine_.has_value()) return false;
        const auto& schema = self->engine_->port_schema();
        if (index >= static_cast<uint32_t>(schema.size())) return false;
        const auto& e       = schema.entries[index];
        info->id            = static_cast<clap_id>(e.id);
        info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
        info->cookie        = nullptr;
        info->min_value     = 0.0;
        info->max_value     = 1.0;
        info->default_value = 0.0;
        const char* slash = std::strrchr(e.name.c_str(), '/');
        const char* leaf  = slash ? slash + 1 : e.name.c_str();
        std::snprintf(info->name,   sizeof(info->name),   "%s", leaf);
        std::snprintf(info->module, sizeof(info->module), "kairos-grid");
        return true;
    }

    static bool params_get_value(const clap_plugin_t* p, clap_id param_id, double* out) {
        const auto* self = cast(p);
        const auto  idx  = static_cast<std::size_t>(param_id);
        if (!self->engine_.has_value() || idx >= self->param_frame_.size()) return false;
        *out = static_cast<double>(self->param_frame_[idx]);
        return true;
    }

    static bool params_value_to_text(const clap_plugin_t* /*p*/, clap_id /*id*/,
                                      double value, char* buf, uint32_t cap) {
        std::snprintf(buf, cap, "%.4f", value);
        return true;
    }

    static bool params_text_to_value(const clap_plugin_t* /*p*/, clap_id /*id*/,
                                      const char* text, double* out) {
        char* end = nullptr;
        *out = std::strtod(text, &end);
        return end != text;
    }

    static void params_flush(const clap_plugin_t* p,
                              const clap_input_events_t*  in,
                              const clap_output_events_t* /*out*/) {
        auto* self = cast_mut(p);
        if (!self->engine_.has_value()) return;
        const uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_event_header_t* hdr = in->get(in, i);
            if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID)
                self->dispatch_event(hdr);
        }
        self->engine_->apply_params(self->param_frame_);
    }

    // -----------------------------------------------------------------------
    // State extension
    //
    // Binary format (all fields little-endian):
    //   uint32_t  magic   = 0x4B475354  ('KGST')
    //   uint32_t  version = 1
    //   uint32_t  n_params
    //   for each param:
    //     uint32_t  name_len
    //     char      name[name_len]   (no null terminator)
    //     float     value
    //
    // load() matches saved entries against the current port_schema by name so
    // it is robust to param additions or removals between saves.
    // -----------------------------------------------------------------------

    static bool stream_write_all(const clap_ostream_t* s, const void* buf, uint64_t n) {
        const auto* p = static_cast<const uint8_t*>(buf);
        uint64_t written = 0;
        while (written < n) {
            const int64_t r = s->write(s, p + written, n - written);
            if (r <= 0) return false;
            written += static_cast<uint64_t>(r);
        }
        return true;
    }

    static bool stream_read_all(const clap_istream_t* s, void* buf, uint64_t n) {
        auto* p = static_cast<uint8_t*>(buf);
        uint64_t done = 0;
        while (done < n) {
            const int64_t r = s->read(s, p + done, n - done);
            if (r <= 0) return false;
            done += static_cast<uint64_t>(r);
        }
        return true;
    }

    static bool state_save(const clap_plugin_t* p, const clap_ostream_t* s) {
        const auto* self = cast(p);
        if (!self->engine_.has_value()) return false;

        constexpr uint32_t k_magic   = 0x4B475354u;
        constexpr uint32_t k_version = 1u;
        const auto& schema  = self->engine_->port_schema();
        const auto  n_params = static_cast<uint32_t>(schema.size());

        if (!stream_write_all(s, &k_magic,   sizeof(k_magic)))   return false;
        if (!stream_write_all(s, &k_version, sizeof(k_version))) return false;
        if (!stream_write_all(s, &n_params,  sizeof(n_params)))  return false;

        for (const auto& e : schema.entries) {
            const auto  name_len = static_cast<uint32_t>(e.name.size());
            const float value    = (static_cast<std::size_t>(e.id) < self->param_frame_.size())
                                       ? self->param_frame_[static_cast<std::size_t>(e.id)]
                                       : 0.f;
            if (!stream_write_all(s, &name_len,     sizeof(name_len))) return false;
            if (name_len > 0 &&
                !stream_write_all(s, e.name.data(), name_len))         return false;
            if (!stream_write_all(s, &value,        sizeof(value)))    return false;
        }
        return true;
    }

    static bool state_load(const clap_plugin_t* p, const clap_istream_t* s) {
        auto* self = cast_mut(p);
        if (!self->engine_.has_value()) return false;

        constexpr uint32_t k_magic   = 0x4B475354u;
        constexpr uint32_t k_version = 1u;

        uint32_t magic{}, version{}, n_params{};
        if (!stream_read_all(s, &magic,    sizeof(magic)))    return false;
        if (!stream_read_all(s, &version,  sizeof(version)))  return false;
        if (magic != k_magic || version != k_version)         return false;
        if (!stream_read_all(s, &n_params, sizeof(n_params))) return false;

        std::string name_buf;
        for (uint32_t i = 0; i < n_params; ++i) {
            uint32_t name_len{};
            if (!stream_read_all(s, &name_len, sizeof(name_len))) return false;
            name_buf.resize(name_len);
            if (name_len > 0 &&
                !stream_read_all(s, name_buf.data(), name_len))   return false;
            float value{};
            if (!stream_read_all(s, &value, sizeof(value)))       return false;
            self->set_param(self->find_port(name_buf.c_str()), value);
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void build_engine(float sr) {
        GridGraph g;
        g.add_module(std::make_unique<EnvironmentModule>());

        auto audio_in_up  = std::make_unique<AudioInputModule>();
        auto audio_out_up = std::make_unique<AudioOutputModule>();
        AudioInputModule*  raw_in  = audio_in_up.get();
        AudioOutputModule* raw_out = audio_out_up.get();
        const int in_idx  = g.add_module(std::move(audio_in_up));
        const int out_idx = g.add_module(std::move(audio_out_up));

        // Stereo pass-through cable — replaced by DSP modules in future graphs.
        g.add_cable({in_idx, AudioInputModule::k_left,  out_idx, AudioOutputModule::k_left});
        g.add_cable({in_idx, AudioInputModule::k_right, out_idx, AudioOutputModule::k_right});

        auto res = g.build();
        if (!res.has_value()) return;
        engine_    = std::move(*res);
        audio_in_  = raw_in;
        audio_out_ = raw_out;
        engine_->prepare(sr);
        param_frame_.assign(static_cast<std::size_t>(engine_->port_schema().size()), 0.f);
        cache_port_ids();
    }

    void cache_port_ids() {
        env_tempo_id_    = find_port("env/tempo_hz");
        env_beat_id_     = find_port("env/beat_phase");
        env_bar_id_      = find_port("env/bar_phase");
        env_playing_id_  = find_port("env/is_playing");
        env_note_id_     = find_port("env/voice_note");
        env_gate_id_     = find_port("env/voice_gate");
        env_velocity_id_ = find_port("env/voice_velocity");
    }

    int find_port(const char* name) const {
        if (!engine_.has_value()) return -1;
        for (const auto& e : engine_->port_schema().entries)
            if (e.name == name) return e.id;
        return -1;
    }

    void set_param(int id, float value) {
        if (id >= 0 && static_cast<std::size_t>(id) < param_frame_.size())
            param_frame_[static_cast<std::size_t>(id)] = value;
    }

    void apply_transport(const clap_event_transport_t* t) {
        if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
            set_param(env_tempo_id_, static_cast<float>(t->tempo / 60.0));

        set_param(env_playing_id_, (t->flags & CLAP_TRANSPORT_IS_PLAYING) ? 1.f : 0.f);

        if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
            const double pos = static_cast<double>(t->song_pos_beats) / CLAP_BEATTIME_FACTOR;
            set_param(env_beat_id_, static_cast<float>(pos - std::floor(pos)));

            if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
                const double bar_start    = static_cast<double>(t->bar_start) / CLAP_BEATTIME_FACTOR;
                const double beats_per_bar = static_cast<double>(t->tsig_num);
                if (beats_per_bar > 0.0) {
                    const double bar_phase = (pos - bar_start) / beats_per_bar;
                    set_param(env_bar_id_,
                              static_cast<float>(std::clamp(bar_phase, 0.0, 1.0)));
                }
            }
        }
    }

    void dispatch_event(const clap_event_header_t* hdr) {
        switch (hdr->type) {
            case CLAP_EVENT_TRANSPORT:
                apply_transport(reinterpret_cast<const clap_event_transport_t*>(hdr));
                break;
            case CLAP_EVENT_NOTE_ON: {
                const auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
                set_param(env_note_id_,     static_cast<float>(ev->key));
                set_param(env_velocity_id_, static_cast<float>(ev->velocity * 127.0));
                set_param(env_gate_id_,     1.f);
                break;
            }
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE:
                set_param(env_gate_id_, 0.f);
                break;
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
                set_param(static_cast<int>(ev->param_id), static_cast<float>(ev->value));
                break;
            }
            default: break;
        }
    }

    // -----------------------------------------------------------------------
    // Trampolines (clap_plugin_t function pointers → member functions)
    // -----------------------------------------------------------------------

    static const KairosGridPlugin* cast(const clap_plugin_t* p) {
        return static_cast<const KairosGridPlugin*>(p->plugin_data);
    }
    static KairosGridPlugin* cast_mut(const clap_plugin_t* p) {
        return static_cast<KairosGridPlugin*>(p->plugin_data);
    }

    static bool s_init(const clap_plugin_t* p) { return cast_mut(p)->init(); }
    static void s_destroy(const clap_plugin_t* p) { cast_mut(p)->destroy(); }
    static bool s_activate(const clap_plugin_t* p, double sr, uint32_t mn, uint32_t mx)
        { return cast_mut(p)->activate(sr, mn, mx); }
    static void s_deactivate(const clap_plugin_t* p)       { cast_mut(p)->deactivate(); }
    static bool s_start_processing(const clap_plugin_t* p) { return cast_mut(p)->start_processing(); }
    static void s_stop_processing(const clap_plugin_t* p)  { cast_mut(p)->stop_processing(); }
    static void s_reset(const clap_plugin_t* p)            { cast_mut(p)->reset(); }
    static clap_process_status s_process(const clap_plugin_t* p, const clap_process_t* proc)
        { return cast_mut(p)->process(proc); }
    static const void* s_get_extension(const clap_plugin_t* p, const char* id)
        { return cast(p)->get_extension(id); }
    static void s_on_main_thread(const clap_plugin_t* p) { cast_mut(p)->on_main_thread(); }

    // -----------------------------------------------------------------------
    // Static extension vtables
    // -----------------------------------------------------------------------

    static const clap_plugin_audio_ports_t s_audio_ports_ext;
    static const clap_plugin_params_t      s_params_ext;
    static const clap_plugin_state_t       s_state_ext;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    [[maybe_unused]] const clap_host_t* host_;
    clap_plugin_t             clap_plugin_{};
    std::optional<GridEngine> engine_;
    AudioInputModule*         audio_in_ {nullptr};
    AudioOutputModule*        audio_out_{nullptr};
    std::vector<float>        param_frame_;
    float                     sample_rate_{48000.f};

    int env_tempo_id_    {-1};
    int env_beat_id_     {-1};
    int env_bar_id_      {-1};
    int env_playing_id_  {-1};
    int env_note_id_     {-1};
    int env_gate_id_     {-1};
    int env_velocity_id_ {-1};
};

const clap_plugin_audio_ports_t KairosGridPlugin::s_audio_ports_ext = {
    .count = &KairosGridPlugin::audio_ports_count,
    .get   = &KairosGridPlugin::audio_ports_get,
};

const clap_plugin_params_t KairosGridPlugin::s_params_ext = {
    .count          = &KairosGridPlugin::params_count,
    .get_info       = &KairosGridPlugin::params_get_info,
    .get_value      = &KairosGridPlugin::params_get_value,
    .value_to_text  = &KairosGridPlugin::params_value_to_text,
    .text_to_value  = &KairosGridPlugin::params_text_to_value,
    .flush          = &KairosGridPlugin::params_flush,
};

const clap_plugin_state_t KairosGridPlugin::s_state_ext = {
    .save = &KairosGridPlugin::state_save,
    .load = &KairosGridPlugin::state_load,
};

// ---------------------------------------------------------------------------
// Plugin factory
// ---------------------------------------------------------------------------

static uint32_t factory_count(const clap_plugin_factory_t*) { return 1; }

static const clap_plugin_descriptor_t* factory_get_descriptor(
    const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &k_descriptor : nullptr;
}

static const clap_plugin_t* factory_create(
    const clap_plugin_factory_t*,
    const clap_host_t* host,
    const char* plugin_id)
{
    if (std::strcmp(plugin_id, k_descriptor.id) != 0) return nullptr;
    return (new KairosGridPlugin(host))->clap_plugin();
}

static const clap_plugin_factory_t k_factory = {
    .get_plugin_count      = &factory_count,
    .get_plugin_descriptor = &factory_get_descriptor,
    .create_plugin         = &factory_create,
};

} // namespace kairos_grid

// ---------------------------------------------------------------------------
// CLAP entry point (C linkage, visible to the host's dynamic linker)
// ---------------------------------------------------------------------------

extern "C" {

static bool   entry_init(const char* /*plugin_path*/) { return true; }
static void   entry_deinit()                          {}
static const void* entry_get_factory(const char* factory_id) {
    if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &kairos_grid::k_factory;
    return nullptr;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init         = &entry_init,
    .deinit       = &entry_deinit,
    .get_factory  = &entry_get_factory,
};

} // extern "C"
