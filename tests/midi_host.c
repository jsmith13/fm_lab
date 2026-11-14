/* SPDX-License-Identifier: GPL-3.0-only */
#define DINGHY_HOST
#define main fm_emulator_main
#include "../firmware/src/fmlab.c"
#undef main
static uint32_t failures;
#define CHECK(test, message) do { if (!(test)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,message); failures++; } } while (0)

static void reset(void)
{
    static const uint8_t fx_defaults[16] = {FX_DEFAULTS};
    mi_init(); ev_w = ev_r = ev_overflow = ages = mheld = monomode = midi_sustain = 0;
    memset(dxv,0,sizeof dxv); memset(vst,0,sizeof vst); memset(genv,0,sizeof genv);
    memset(&xs,0,sizeof xs); xs.ph_a = PH_A[PH_LO >> 8];
    memset(ui,0,sizeof ui); memcpy(&ui[UI_FX],fx_defaults,sizeof fx_defaults);
    ui[UI_GDEC] = 50; ui[UI_GSUS] = ui[UI_GREL] = 99; ui[UI_GOFF] = 1; ui[UI_FCUT] = 60;
    memset(sq_left,0,sizeof sq_left); seq_run = arp_on = rec = chord_n = arpkeys = rkeys = 0;
    sq_n = 0; arp_note = 0xFFu; clk_a = 0; midi_silence = 1;
    opmute = pcur = modified = 0; dx_os = os_want = 0; sr_hz = FS; crush_sh = out_mono = 0;
    dx_init_voice(edit); memcpy(other,edit,sizeof edit); patch_publish();
    glo_sync(); flt_sync(); fx_sync(); pl_sync();
}

static void audio(uint32_t blocks)
{
    int32_t out[512];
    while (blocks--) { memset(out,0,sizeof out); app_audio(out,256); }
}

static void send(uint32_t st, uint32_t d1, uint32_t d2) { fm_midi_dispatch(st,d1,d2); }

int main(void)
{
    uint32_t i, held, sustained, phase, delay;
    uint8_t base[DX_VSZ];
    mi_config_t mapping;
    settings_t stored, restored;
    int32_t out[512];
    reset(); send(0xB0,64,63); send(0x90,60,100); audio(1); send(0x80,60,0); audio(1);
    CHECK(!midi_sustain && !vst[0].sustained,"CC64 value 63 uses normal operator release");
    reset(); send(0xB0,64,64);
    for (i = 0; i < NV; i++) send(0x90,60+i,100);
    audio(1);
    for (i = 0; i < NV; i++) send(0x80,60+i,0);
    audio(1);
    sustained = 0; for (i = 0; i < NV; i++) sustained += vst[i].sustained;
    CHECK(sustained == NV && v_lit == 255u,"all eight voices sustain until pedal release");
    send(0x90,60,110); audio(1);
    held = sustained = 0;
    for (i = 0; i < NV; i++) { held += vst[i].held; sustained += vst[i].sustained; }
    CHECK(held == 1u && sustained == 7u,"repeated pedal-held note reuses one voice");
    send(0x90,90,100); audio(1);
    CHECK(vst[0].held && vst[0].note == 60u,"stealing favors sustained over physically held voices");
    mapping = mi_config; edit[134] = 31; patch_publish(); audio(1);
    CHECK(midi_sustain && !memcmp(&mapping,&mi_config,sizeof mapping),"patch change preserves global pedal/mapping state");
    send(0xB0,64,0); send(0x80,60,0); send(0x90,90,0); audio(80);
    CHECK(!v_lit,"repeated/stolen notes finish release after a patch change");
    for (i = 0; i < NV; i++) CHECK(!vst[i].sustained,"pedal release clears every deferred key-up");

    reset(); ev_push(EV_MONO,1,0); audio(1);
    send(0xB0,64,127); send(0x90,60,100); send(0x90,64,100); audio(1);
    send(0x80,64,0); audio(1); CHECK(vst[0].note == 60u && vst[0].held,"mono fallback remains physical last-note priority");
    send(0x80,60,0); audio(1); CHECK(vst[0].sustained && !mheld,"mono last key sustains");
    send(0xB0,121,0); audio(80); CHECK(!v_lit && !midi_sustain,"reset controllers releases mono pedal ownership");

    reset(); memcpy(base,edit,sizeof base);
    send(0xE0,0,0); audio(4); CHECK(mi_pitch() == -512,"negative 14-bit bend endpoint");
    send(0xE0,127,127); audio(4); CHECK(mi_pitch() == 512,"positive 14-bit bend endpoint");
    send(0xE0,0,64); audio(4); CHECK(!mi_pitch(),"bend center is exact");
    mi_range_turn(10); send(0xE0,127,127); audio(4); CHECK(mi_pitch() == 12*256,"adjustable bend range");
    send(0x90,60,100); audio(1); send(0xB0,1,127); audio(4);
    CHECK(dxv[0].controller_vibrato > 0,"wheel adds patch-LFO vibrato");
    send(0xB0,1,0); audio(4); CHECK(!dxv[0].controller_vibrato,"zero wheel restores programmed modulation");
    send(0xD0,127,255); audio(4);
    CHECK(dxv[0].bright > 0 && dxv[0].controller_vibrato > 0,"one-byte pressure brightens and adds vibrato together");
    mi_config.route[9].negative = 1; mi_publish(); audio(4);
    CHECK(dxv[0].bright < 0,"channel pressure supports adjustable polarity");
    mi_slot = 10; mi_group_turn(2); mi_toggle_learn(); send(0xB0,21,20); audio(4);
    phase = dxv[0].lfo.phase; delay = dxv[0].lfo.delaystate;
    send(0xB0,21,100); mi_tick(64);
    { flt_t f; fx_t x; fm_midi_prepare(&f,&x); }
    CHECK(dxv[0].lfo.phase == phase && dxv[0].lfo.delaystate == delay,
          "temporary operator updates preserve LFO phase and delay progress");
    CHECK(!memcmp(edit,base,sizeof base) && !modified,"live modulation leaves native sound/wave bytes unmodified");
    opmute = 1u << 5; patch_publish(); audio(4);
    CHECK(!midi_voice[5u*21u+16u] && (dxv[0].controller_mutes & (1u << 5)),
          "controller level/brightness routing cannot unmute a switched-off operator");

    reset(); audio(1); send(0xB0,74,127); audio(1);
    CHECK(mi_value(MI_CUTOFF,60*256,0,99*256) > 60*256 &&
          mi_value(MI_CUTOFF,60*256,0,99*256) < 99*256,"first filter CC crossfades from the saved base value");
    audio(4); send(0xB0,71,127); send(0xB0,91,100); send(0xB0,93,80); audio(4);
    { const param_t cutoff = {"CUTOFF",0,P_UI,UI_FCUT,99,K_HZ};
      p_turn(&cutoff,-1); glo_sync(); audio(4);
      CHECK(mi_value(MI_CUTOFF,ui[UI_FCUT]*256,0,99*256) == ui[UI_FCUT]*256,
            "front-panel parameter edits take control back from MIDI"); }
    send(0xB0,74,127); audio(4);
    CHECK(mi_value(MI_CUTOFF,ui[UI_FCUT]*256,0,99*256) == 99*256,
          "the next MIDI movement reactivates the mapping");
    { flt_t f; fx_t x; fm_midi_prepare(&f,&x);
      CHECK(x.rv_mix > 0 && x.ch_mix > 0 && (x.on & 9u) == 9u,"CC91/93 enable live effects without editing global settings"); }
    mapping = mi_config;
    send(0xB0,7,63); send(0xB0,10,0); send(0xB0,11,0); send(0xD0,100,0); audio(4);
    send(0xB0,121,0); audio(4);
    CHECK(mi_value(MI_VOLUME,32768,0,32768) < 17000 && mi_value(MI_PAN,32768,0,65536) == 0 &&
          mi_value(MI_EXPRESSION,32768,0,32768) == 32768 && !mi_pitch() &&
          !memcmp(&mapping,&mi_config,sizeof mapping),"controller reset preserves volume/pan/mappings and restores expressive controls");
    send(0xB0,11,0); audio(4); CHECK(!mi_value(MI_EXPRESSION,32768,0,32768),"expression can reach silence");

    reset(); out_mono = 1; send(0xB0,10,127); send(0x90,60,100); audio(4);
    memset(out,0,sizeof out); app_audio(out,256); held = 0;
    for (i = 0; i < 256u; i++) {
        CHECK(out[2u*i] == out[2u*i+1u],"OUTPUT MONO folds pan onto both channels");
        held |= (uint32_t)out[2u*i];
    }
    CHECK(held,"fully right-panned notes remain audible through a mono plug");
    reset(); audio(1);
    mi_config.route[10] = (mi_route_t){MI_PRESSURE,MI_AMPLITUDE,25,1};
    mi_publish(); send(0xD0,127,0); audio(4);
    CHECK(mi_left.current == 16384 && mi_right.current == 16384,
          "negative pressure amplitude depth reaches the stereo gain path");
    send(0xD0,0,0); audio(4);
    CHECK(mi_left.current == 32768 && mi_right.current == 32768,"zero pressure adds no amplitude modulation");
    mi_config.route[10] = (mi_route_t){MI_PRESSURE,MI_CUTOFF,50,1};
    mi_publish(); send(0xD0,127,0); audio(4);
    CHECK(mi_value(MI_CUTOFF,60*256,0,99*256) == 2688,"pressure also modulates cutoff with depth and polarity");
    reset(); send(0xB0,64,127); send(0x90,60,100); audio(1); send(0xB0,123,0); audio(1);
    CHECK(v_lit && vst[0].sustained && !vst[0].held,"All Notes Off honors sustain");
    send(0xB0,64,0); audio(80); CHECK(!v_lit,"All Notes Off finishes normal release on pedal-up");
    send(0xB0,91,100); send(0xB0,93,100); send(0x90,64,100); audio(80);
    send(0xB0,64,127); send(0xB0,120,0); memset(out,0,sizeof out); app_audio(out,256);
    for (i = 0; i < 512u; i++) CHECK(!out[i],"All Sound Off clears voices, decimator, filter and effect tails immediately");
    CHECK(midi_sustain,"All Sound Off preserves the pedal/controller setting");
    send(0x90,60,100); send(0x80,60,0); audio(1);
    CHECK(vst[0].sustained,"new notes honor the preserved pedal after hard silence");
    send(0xB0,121,0); audio(80); CHECK(!v_lit,"reset controllers releases the preserved pedal");

    reset(); ui[UI_MCH] = 3; send(0xB0,64,127); send(0x90,60,100); audio(1);
    CHECK(!v_lit && !midi_sustain,"channel filtering includes note and controller messages");
    send(0xB2,64,127); send(0x92,60,100); audio(1);
    CHECK(v_lit && midi_sustain,"selected channel accepts notes and controllers");
    ui[UI_MCH] = 1; glo_sync(); audio(1);
    CHECK(!v_lit && !midi_sustain,"receive-channel changes clear old notes and pedal ownership");
    reset(); for (i = 0; i < EVQ+2u; i++) send(0x90,i,100);
    CHECK(ev_overflow,"event overflow is explicitly reported"); audio(1);
    CHECK(!ev_overflow && !v_lit && !midi_sustain,"overflow cannot strand a note or pedal-up");

    reset(); memset(&stored,0,sizeof stored); set_take(&stored);
    memset(host_store,0,sizeof host_store); settings_save(&stored,FM_LEGACY_SETTINGS_BYTES);
    memset(&restored,0,sizeof restored); mi_defaults(&restored.midi);
    CHECK(!settings_load(&restored,sizeof restored) && mi_default_config(&restored.midi),
          "legacy short settings retain defaults for appended MIDI mappings");
    mi_slot = 10; mi_group_turn(1); mi_toggle_learn(); send(0xB0,0,2);
    CHECK(mi_learn && mi_config.route[10].source == MI_OFF,"bank select remains fixed and cannot be learned");
    send(0xB0,21,100); mi_knob(1,-35); mi_knob(2,-1); mi_knob(3,5);
    CHECK(!mi_learn && mi_config.route[10].source == 21u && mi_config.route[10].depth == 65u,
          "Learn, destination, depth and polarity edit the selected global route");
    mapping = mi_config; set_take(&stored);
    CHECK(!fm_settings_write(&stored),"extended settings verify the written record"); mi_init();
    memset(&restored,0,sizeof restored); CHECK(!settings_load(&restored,sizeof restored),"extended host settings load without byte-length truncation");
    CHECK(mi_valid(&restored.midi) && !memcmp(&restored.midi,&mapping,sizeof mapping),
          "global assignment/depth/polarity/bend range survives settings reload");
    settings_save(&stored,FM_LEGACY_SETTINGS_BYTES+5u);
    memset(&restored,0,sizeof restored); mi_defaults(&restored.midi);
    CHECK(!settings_load(&restored,sizeof restored) && !fm_settings_midi_valid(&restored),
          "partial appended controller records cannot silently inherit default-filled routes");
    CHECK(!mi_input.bend && !mi_input.value[21],"reboot does not restore live controllers");
    restored.midi.route[10].source = 64; CHECK(!mi_valid(&restored.midi),"stored reserved sources are rejected");
    mi_slot = 10; mi_group_turn(1); mi_toggle_learn(); send(0xD0,100,0);
    CHECK(mi_config.route[10].source == MI_PRESSURE && !mi_learn,"channel pressure can be learned separately from CC");
    {
        uint8_t seen[MI_TARGET_COUNT] = {0};
        uint8_t saved_target = mi_config.route[10].target;
        uint32_t g, n;
        for (g = 0; g < MI_GROUP_COUNT; g++) {
            CHECK(mi_groups[g][0] >= 3u,"destination groups contain at least three meaningful choices");
            for (n = 1; n <= mi_groups[g][0]; n++) {
                uint32_t target = mi_groups[g][n];
                CHECK(target < MI_TARGET_COUNT && !seen[target],"destination hierarchy preserves unique stored IDs");
                if (target < MI_TARGET_COUNT) {
                    seen[target] = 1; mi_config.route[10].target = (uint8_t)target;
                    mi_drawn = ~0u; mi_draw();
                }
            }
        }
        for (n = 0; n < MI_TARGET_COUNT; n++) CHECK(seen[n],"every stored destination remains reachable");
        mi_config.route[10].target = saved_target; mi_drawn = ~0u;
    }
    mi_open_editor(); mi_toggle_learn(); mapping = mi_config; mi_slot_turn(1);
    CHECK(mi_editor && mi_slot == 11u && !mi_learn && !mi_feedback &&
          !memcmp(&mi_config,&mapping,sizeof mapping),"slot paging cancels Learn without changing settings");
    mi_slot = 15; mi_slot_turn(1); CHECK(!mi_slot,"assignment paging wraps to the first slot");
    mi_slot_turn(10); mi_target_turn(-1);
    CHECK(mi_config.route[10].target == 51u && mi_config.bend_range == mapping.bend_range,
          "ALGORITHM wraps within TONE without editing the bend range");
    mi_group_turn(1); CHECK(mi_config.route[10].target == 13u,"PRESETS selects the next group's first target");
    mi_knob(3,100); CHECK(mi_config.bend_range == 24u,"BEND knob clamps to the global maximum");
    mi_knob(3,-100); CHECK(!mi_config.bend_range,"BEND knob clamps to the global minimum");
    reset(); page = PAGE_HOME; focus = FOC_OFF;
    page_to(PAGE_GLO); sel_step((int32_t)page_nbank()-1);
    CHECK(glo_diagnostics() && !bank_has_op() && !page_at(bank_first(bank)),
          "GLOBAL ends with a read-only diagnostics bank, not an operator or patch parameter");
    draw_diagnostics(3);
    page_to(PAGE_GLO); CHECK(!bank && !glo_diagnostics(),"GLO wraps diagnostics to the first settings bank");
    sel_step(-1); CHECK(glo_diagnostics(),"SELECT reaches diagnostics from either direction");
    {
        uint32_t budget = BUDGET_US*FM1_TICKS_PER_US;
        cpu_profile_t parts = {0,budget/4u,budget/8u,budget/16u,budget/16u};
        cpu_profile_t saved;
        cpu_reset_generation = cpu_reset_seen = cpu_peak = cpu_q8 = cpu_late = cpu_over = 0;
        memset(cpu_profiles,0,sizeof cpu_profiles);
        cpu_report(budget/2u,&parts); saved = cpu_profiles[cpu_profile_index];
        CHECK(cpu_peak == 50u && saved.percent == 50u && saved.synth == 25u && saved.fx == 12u &&
              saved.control == 6u && saved.post == 6u && !cpu_late,
              "CPU and stage percentages use the DMA callback budget");
        parts.synth = 0; cpu_report(budget/4u,&parts);
        CHECK(cpu_peak == 50u && !memcmp(&saved,&cpu_profiles[cpu_profile_index],sizeof saved),
              "stage breakdown belongs to the recorded peak callback, not the latest callback");
        cpu_report(budget,&parts); CHECK(!cpu_late,"exact callback deadline is not counted late");
        cpu_report(budget+FM1_TICKS_PER_US,&parts); CHECK(cpu_late == 1u,"over-budget callbacks increment LATE");
        cpu_over = 0xA5u; saved = cpu_profiles[cpu_profile_index];
        cpu_request_reset();
        CHECK(cpu_peak == saved.percent && cpu_late == 1u,
              "panel reset requests never overwrite audio-owned measurements");
        cpu_reset_pending();
        CHECK(!cpu_peak && !cpu_late && !cpu_profiles[0].percent && !cpu_profiles[1].percent &&
              cpu_over == 0xA5u && cpu_q8,
              "audio applies peak/LATE reset without resetting current load or overload protection");
        cpu_report(budget/4u,&parts); CHECK(cpu_peak == 25u,"measurements resume after reset");
        reset(); send(0x90,60,100); audio(1); memcpy(base,edit,sizeof base);
        cpu_request_reset(); audio(1);
        CHECK(v_lit && !opmute && !memcmp(base,edit,sizeof base),
              "diagnostic reset during audio cannot mute a voice or alter its patch");
        cpu_over = 0;
        cpu_report(budget,&parts); CHECK(dxv[0].on,"one expensive callback preserves the voice");
        cpu_report(budget,&parts); CHECK(!dxv[0].on,"two expensive callbacks retain existing voice-shedding protection");
    }
    CHECK(g_plot_ratio(-G_ONE,232,G_ONE/99) == (int32_t)(-(int64_t)G_ONE*232/(G_ONE/99)),
          "global-envelope cursor scaling preserves signed Q24 extremes without 64-bit division");
    printf("FM MIDI controller checks: %u failures\n",failures);
    return failures ? 1 : 0;
}
