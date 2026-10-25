#include "test.h"
#include "fluidsynth.h"
#include "fluid_synth.h"
#include "fluid_voice.h"
#include "fluid_rvoice.h"
#include "fluid_rvoice_event.h"
#include <math.h>

typedef struct
{
    fluid_settings_t *settings;
    fluid_synth_t *synth;
    fluid_sample_t *sample;
    fluid_voice_t *voice;
} fixture_t;

static void assert_close(fluid_real_t actual, fluid_real_t expected)
{
    if(FLUID_FABS(actual - expected) >= 0.00001)
    {
        fprintf(stderr, "actual=%g expected=%g\n", (double)actual, (double)expected);
    }
    TEST_ASSERT(FLUID_FABS(actual - expected) < 0.00001);
}

static void start_voice(fixture_t *fixture, int delayed)
{
    fixture->voice = fluid_synth_alloc_voice(fixture->synth, fixture->sample, 0, 60, 127);
    TEST_ASSERT(fixture->voice != NULL);
    fluid_voice_gen_set(fixture->voice, GEN_SAMPLEMODE, 1);
    fluid_voice_gen_set(fixture->voice, GEN_VOLENVDELAY, delayed ? 1200 : -32768);
    fluid_voice_gen_set(fixture->voice, GEN_VOLENVATTACK, -32768);
    fluid_synth_start_voice(fixture->synth, fixture->voice);
}

static void setup(fixture_t *fixture, int delayed)
{
    short data[256];
    int i;

    fixture->settings = new_fluid_settings();
    TEST_ASSERT(fixture->settings != NULL);
    TEST_SUCCESS(fluid_settings_setnum(fixture->settings, "synth.sample-rate", 48000));
    TEST_SUCCESS(fluid_settings_setint(fixture->settings, "synth.polyphony", 1));
    TEST_SUCCESS(fluid_settings_setint(fixture->settings, "synth.reverb.active", 0));
    TEST_SUCCESS(fluid_settings_setint(fixture->settings, "synth.chorus.active", 0));
    fixture->synth = new_fluid_synth(fixture->settings);
    fixture->sample = new_fluid_sample();
    TEST_ASSERT(fixture->synth != NULL && fixture->sample != NULL);
    for(i = 0; i < 256; i++)
    {
        data[i] = i % 32 < 16 ? 10000 : -10000;
    }
    TEST_SUCCESS(fluid_sample_set_sound_data(fixture->sample, data, NULL, 256, 48000, 1));
    TEST_SUCCESS(fluid_sample_set_pitch(fixture->sample, 60, 0));
    TEST_SUCCESS(fluid_sample_set_loop(fixture->sample, 8, 248));
    start_voice(fixture, delayed);
}

static void teardown(fixture_t *fixture)
{
    delete_fluid_synth(fixture->synth);
    delete_fluid_sample(fixture->sample);
    delete_fluid_settings(fixture->settings);
}

static void render_block(fixture_t *fixture)
{
    float left[FLUID_BUFSIZE], right[FLUID_BUFSIZE];
    TEST_SUCCESS(fluid_synth_write_float(fixture->synth, FLUID_BUFSIZE,
                                        left, 0, 1, right, 0, 1));
}

static void configure(fixture_t *fixture, int slot, fluid_real_t pressure,
                      fluid_real_t duration)
{
    fluid_mod_t mod = {0};
    mod.dest = GEN_ATTENUATION;
    mod.src1 = FLUID_MOD_CHANNELPRESSURE;
    mod.amount = 128.0;
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(fixture->voice->eventhandler,
        &fixture->voice->rvoice->attenuation_mods[slot], &mod, pressure, 128.0, duration));
}

static void set_base(fixture_t *fixture, fluid_real_t base, int count)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};
    param[0].real = base;
    param[1].i = count;
    TEST_SUCCESS(fluid_rvoice_eventhandler_push(fixture->voice->eventhandler,
        fluid_rvoice_set_slew_attenuation, fixture->voice->rvoice, param));
    fluid_rvoice_eventhandler_flush(fixture->voice->eventhandler);
}

static void test_blocks_and_reuse(void)
{
    fixture_t fixture;
    fluid_rvoice_t *rvoice;
    fluid_voice_t *old_voice;
    int i;

    setup(&fixture, 0);
    rvoice = fixture.voice->rvoice;
    configure(&fixture, 0, 64.0, 256.0);
    set_base(&fixture, 100.0, 1);
    render_block(&fixture);
    /* Initial pressure is adopted directly: no artificial note-on fade. */
    assert_close(rvoice->dsp.attenuation, 164.0);
    TEST_ASSERT(rvoice->attenuation_mod_count == 1);

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(fixture.voice->eventhandler,
        &rvoice->attenuation_mods[0], 127.0, 128.0));
    fluid_rvoice_eventhandler_flush(fixture.voice->eventhandler);
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 195.75);
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 227.0);

    set_base(&fixture, 200.0, 1);
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 327.0);
    TEST_ASSERT(rvoice->dsp.min_attenuation_cB == 0.0);

    /* End the old voice, then reuse its synthesis-voice allocation. */
    old_voice = fixture.voice;
    TEST_SUCCESS(fluid_synth_all_sounds_off(fixture.synth, 0));
    render_block(&fixture);
    start_voice(&fixture, 0);
    TEST_ASSERT(fixture.voice == old_voice);
    render_block(&fixture);
    rvoice = fixture.voice->rvoice;
    TEST_ASSERT(rvoice->attenuation_mod_count == 0);
    for(i = 0; i < FLUID_RVOICE_MAX_SLEW_MODS; i++)
    {
        TEST_ASSERT(!rvoice->attenuation_mods[i].configured);
    }
    teardown(&fixture);
}

static void test_quiet_block(void)
{
    fixture_t fixture;
    fluid_rvoice_t *rvoice;

    setup(&fixture, 1);
    rvoice = fixture.voice->rvoice;
    configure(&fixture, 0, 0.0, 256.0);
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(fixture.voice->eventhandler,
        &rvoice->attenuation_mods[0], 127.0, 128.0));
    set_base(&fixture, 0.0, 1);
    render_block(&fixture);
    TEST_ASSERT(fluid_adsr_env_get_section(&rvoice->envlfo.volenv) == FLUID_VOICE_ENVDELAY);
    assert_close(rvoice->attenuation_mods[0].source[0].slew.current, 0.25);
    assert_close(rvoice->dsp.attenuation, 31.75);
    teardown(&fixture);
}

static void test_overflow_isolation(void)
{
    fixture_t fixture;
    fluid_rvoice_t *old_rvoice;
    fluid_rvoice_t *new_rvoice;

    setup(&fixture, 0);
    old_rvoice = fixture.voice->rvoice;
    configure(&fixture, 0, 64.0, 256.0);
    set_base(&fixture, 100.0, 1);
    render_block(&fixture);
    assert_close(old_rvoice->dsp.attenuation, 164.0);

    /* Polyphony is one: allocating while the previous note is still active
     * forces the normal/overflow rendering-voice swap.
     */
    start_voice(&fixture, 0);
    new_rvoice = fixture.voice->rvoice;
    TEST_ASSERT(new_rvoice != old_rvoice);
    render_block(&fixture);
    TEST_ASSERT(new_rvoice->attenuation_mod_count == 0);
    TEST_ASSERT(!new_rvoice->attenuation_mods[0].configured);
    TEST_ASSERT(old_rvoice->attenuation_mods[0].configured);

    configure(&fixture, 0, 10.0, 256.0);
    set_base(&fixture, 20.0, 1);
    render_block(&fixture);
    assert_close(new_rvoice->dsp.attenuation, 30.0);
    assert_close(old_rvoice->attenuation_mods[0].source[0].value, 64.0);
    teardown(&fixture);
}

static void test_sum_before_clip_and_rate(void)
{
    fixture_t fixture;
    fluid_rvoice_t *rvoice;
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};

    setup(&fixture, 0);
    rvoice = fixture.voice->rvoice;
    configure(&fixture, 0, 127.0, 256.0);
    configure(&fixture, 1, 64.0, 256.0);
    set_base(&fixture, -100.0, 2);
    render_block(&fixture);
    /* Clipping the baseline before adding routes would incorrectly give 191. */
    assert_close(rvoice->dsp.attenuation, 91.0);
    set_base(&fixture, 1400.0, 2);
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 1440.0);
    set_base(&fixture, -200.0, 2);
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 0.0);

    /* The real output-rate event preserves the duration in physical time. */
    param[0].real = 96000.0;
    TEST_SUCCESS(fluid_rvoice_eventhandler_push(fixture.voice->eventhandler,
        fluid_rvoice_set_output_rate, rvoice, param));
    fluid_rvoice_eventhandler_flush(fixture.voice->eventhandler);
    render_block(&fixture);
    assert_close(rvoice->attenuation_mods[0].full_range_samples, 512.0);

    /* Returning to the legacy full-attenuation event disables the split path. */
    param[0].real = 42.0;
    TEST_SUCCESS(fluid_rvoice_eventhandler_push(fixture.voice->eventhandler,
        fluid_rvoice_set_attenuation, rvoice, param));
    fluid_rvoice_eventhandler_flush(fixture.voice->eventhandler);
    render_block(&fixture);
    TEST_ASSERT(rvoice->attenuation_mod_count == 0);
    assert_close(rvoice->dsp.attenuation, 42.0);
    teardown(&fixture);
}

static void prepare_api_fixture(fixture_t *fixture, double ms)
{
    setup(fixture, 0);
    TEST_SUCCESS(fluid_synth_all_sounds_off(fixture->synth, 0));
    render_block(fixture);
    TEST_SUCCESS(fluid_synth_cc(fixture->synth, 0, 7, 127));
    TEST_SUCCESS(fluid_settings_setnum(fixture->settings,
                                      "synth.pressure-attenuation-slew-ms", ms));
}

static fluid_voice_t *start_pressure_voice(fixture_t *fixture, int key, int source)
{
    fluid_mod_t mod = {0};
    fluid_voice_t *voice = fluid_synth_alloc_voice(fixture->synth, fixture->sample, 0, key, 127);
    TEST_ASSERT(voice != NULL);
    fluid_voice_gen_set(voice, GEN_SAMPLEMODE, 1);
    fluid_voice_gen_set(voice, GEN_VOLENVDELAY, -32768);
    fluid_voice_gen_set(voice, GEN_VOLENVATTACK, -32768);
    mod.dest = GEN_ATTENUATION;
    mod.src1 = source;
    mod.amount = 128.0;
    fluid_voice_add_mod(voice, &mod, FLUID_VOICE_DEFAULT);
    fluid_synth_start_voice(fixture->synth, voice);
    fixture->voice = voice;
    return voice;
}

static void test_channel_pressure_api(void)
{
    fixture_t fixture;
    fluid_voice_t *voice;
    fluid_rvoice_t *rvoice;
    const char *setting = "synth.pressure-attenuation-slew-ms";

    prepare_api_fixture(&fixture, 256.0 * 1000.0 / 48000.0);
    TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 64));
    voice = start_pressure_voice(&fixture, 60, FLUID_MOD_CHANNELPRESSURE);
    rvoice = voice->rvoice;
    render_block(&fixture);
    TEST_ASSERT(rvoice->attenuation_mod_count == 1);
    assert_close(rvoice->dsp.attenuation, 64.0);

    TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 127));
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 95.75);
    /* The unselected default pressure->vibrato route is immediate. */
    assert_close(fluid_voice_gen_value(voice, GEN_VIBLFOTOPITCH), 50.0 * 127.0 / 128.0);

    TEST_SUCCESS(fluid_settings_setnum(fixture.settings, setting, 512.0 * 1000.0 / 48000.0));
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 111.625);

    TEST_SUCCESS(fluid_settings_setnum(fixture.settings, setting, 0.0));
    render_block(&fixture);
    TEST_ASSERT(rvoice->attenuation_mod_count == 0);
    assert_close(rvoice->dsp.attenuation, 127.0);

    TEST_SUCCESS(fluid_settings_setnum(fixture.settings, setting, 256.0 * 1000.0 / 48000.0));
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 127.0);
    TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 0));
    render_block(&fixture);
    assert_close(rvoice->dsp.attenuation, 95.25);
    TEST_SUCCESS(fluid_synth_all_sounds_off(fixture.synth, 0));
    render_block(&fixture);
    TEST_ASSERT(fluid_synth_get_active_voice_count(fixture.synth) == 0);
    teardown(&fixture);
}

static void test_key_pressure_api(void)
{
    fixture_t fixture;
    fluid_voice_t *first, *second;
    prepare_api_fixture(&fixture, 256.0 * 1000.0 / 48000.0);
    TEST_SUCCESS(fluid_settings_setint(fixture.settings, "synth.polyphony", 4));
    render_block(&fixture);
    first = start_pressure_voice(&fixture, 60, FLUID_MOD_KEYPRESSURE);
    render_block(&fixture);
    second = start_pressure_voice(&fixture, 64, FLUID_MOD_KEYPRESSURE);
    render_block(&fixture);
    TEST_SUCCESS(fluid_synth_key_pressure(fixture.synth, 0, 60, 127));
    render_block(&fixture);
    assert_close(first->rvoice->dsp.attenuation, 31.75);
    assert_close(second->rvoice->dsp.attenuation, 0.0);
    TEST_SUCCESS(fluid_synth_key_pressure(fixture.synth, 0, 64, 64));
    render_block(&fixture);
    assert_close(first->rvoice->dsp.attenuation, 63.5);
    assert_close(second->rvoice->dsp.attenuation, 31.75);
    teardown(&fixture);
}

static void test_api_queue_failure(void)
{
    fixture_t fixture;
    fluid_voice_t *voice;
    fluid_ringbuffer_t *original;
    fluid_rvoice_eventhandler_t *handler;

    prepare_api_fixture(&fixture, 256.0 * 1000.0 / 48000.0);
    voice = start_pressure_voice(&fixture, 60, FLUID_MOD_KEYPRESSURE);
    render_block(&fixture);
    handler = voice->eventhandler;
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_count(handler) == 0);
    TEST_ASSERT(fluid_atomic_int_get(&handler->queue_stored) == 0);
    original = handler->queue;
    handler->queue = new_fluid_ringbuffer(1, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler->queue != NULL);

    /* Updating one selected route plus its subtotal requires two events. */
    TEST_ASSERT(fluid_synth_key_pressure(fixture.synth, 0, 60, 127) == FLUID_FAILED);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_count(handler) == 0);
    TEST_ASSERT(fluid_atomic_int_get(&handler->queue_stored) == 0);
    TEST_ASSERT(voice->pressure_slew_count == 1);
    assert_close(voice->rvoice->attenuation_mods[0].source[0].slew.target, 0.0);

    delete_fluid_ringbuffer(handler->queue);
    handler->queue = original;
    /* The same controller value can be retried after capacity is available. */
    TEST_SUCCESS(fluid_synth_key_pressure(fixture.synth, 0, 60, 127));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation, 31.75);
    teardown(&fixture);
}

static void test_excess_routes_remain_immediate(void)
{
    fixture_t fixture;
    fluid_mod_t mod = {0};
    fluid_voice_t *voice;
    int i;

    prepare_api_fixture(&fixture, 256.0 * 1000.0 / 48000.0);
    for(i = 0; i <= FLUID_NUM_SLEW_MOD; i++)
    {
        TEST_SUCCESS(fluid_synth_cc(fixture.synth, 0, 20 + i, 127));
    }
    voice = fluid_synth_alloc_voice(fixture.synth, fixture.sample, 0, 60, 127);
    TEST_ASSERT(voice != NULL);
    fluid_voice_gen_set(voice, GEN_SAMPLEMODE, 1);
    mod.dest = GEN_ATTENUATION;
    mod.src1 = FLUID_MOD_KEYPRESSURE;
    mod.flags2 = FLUID_MOD_CC;
    mod.amount = 128.0;
    for(i = 0; i <= FLUID_NUM_SLEW_MOD; i++)
    {
        mod.src2 = 20 + i;
        fluid_voice_add_mod(voice, &mod, FLUID_VOICE_DEFAULT);
    }
    fluid_synth_start_voice(fixture.synth, voice);
    render_block(&fixture);
    TEST_ASSERT(voice->pressure_slew_count == FLUID_NUM_SLEW_MOD);
    TEST_ASSERT(voice->rvoice->attenuation_mod_count == FLUID_NUM_SLEW_MOD);
    TEST_SUCCESS(fluid_synth_key_pressure(fixture.synth, 0, 60, 127));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation,
                 (FLUID_NUM_SLEW_MOD * 31.75 + 127.0) * 127.0 / 128.0);

    /* Unselected CC input changes immediately without resetting pressure. */
    TEST_SUCCESS(fluid_synth_cc(fixture.synth, 0, 20, 0));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation,
                 ((FLUID_NUM_SLEW_MOD - 1) * 63.5 + 127.0) * 127.0 / 128.0);
    teardown(&fixture);
}

static void test_expression_y_api(void)
{
    fixture_t fixture;
    fluid_voice_t *voice;
    fluid_mod_t mod = {0};
    int cc;
    for(cc = 1; cc <= 74; cc += 73)
    {
        prepare_api_fixture(&fixture, 256.0 * 1000.0 / 48000.0);
        TEST_SUCCESS(fluid_synth_cc(fixture.synth, 0, cc, 0));
        voice = fluid_synth_alloc_voice(fixture.synth, fixture.sample, 0, 60, 127);
        TEST_ASSERT(voice != NULL);
        mod.dest = GEN_ATTENUATION;
        mod.src1 = cc;
        mod.flags1 = FLUID_MOD_CC;
        mod.amount = 128.0;
        fluid_voice_add_mod(voice, &mod, FLUID_VOICE_DEFAULT);
        mod.src1 = FLUID_MOD_CHANNELPRESSURE;
        mod.flags1 = 0;
        fluid_voice_add_mod(voice, &mod, FLUID_VOICE_DEFAULT);
        fluid_synth_start_voice(fixture.synth, voice);
        fixture.voice = voice;
        render_block(&fixture);
        TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 127));
        render_block(&fixture);
        assert_close(voice->rvoice->dsp.attenuation, 31.75);
        TEST_SUCCESS(fluid_settings_setnum(fixture.settings,
            "synth.expression-y-attenuation-slew-ms", 512.0 * 1000.0 / 48000.0));
        TEST_SUCCESS(fluid_synth_cc(fixture.synth, 0, cc, 127));
        render_block(&fixture);
        /* Pressure continues to 63.5; expression advances independently 15.875. */
        assert_close(voice->rvoice->dsp.attenuation, 79.375);
        TEST_SUCCESS(fluid_settings_setnum(fixture.settings,
            "synth.expression-y-attenuation-slew-ms", 0.0));
        render_block(&fixture);
        /* Expression bypasses immediately; pressure continues its old ramp. */
        assert_close(voice->rvoice->dsp.attenuation, 222.25);
        teardown(&fixture);
    }
}

static void test_minimum_pressure_api(void)
{
    fixture_t fixture;
    fluid_voice_t *voice;
    prepare_api_fixture(&fixture, 250.0);
    TEST_SUCCESS(fluid_settings_setnum(fixture.settings, "synth.attenuation-slew-min-ms", 20.0));
    voice = start_pressure_voice(&fixture, 60, FLUID_MOD_CHANNELPRESSURE);
    render_block(&fixture);
    TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 1));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation, 1.0 / 15.0);
    TEST_SUCCESS(fluid_synth_channel_pressure(fixture.synth, 0, 1));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation, 2.0 / 15.0);
    TEST_SUCCESS(fluid_settings_setnum(fixture.settings, "synth.pressure-attenuation-slew-ms", 0.0));
    render_block(&fixture);
    assert_close(voice->rvoice->dsp.attenuation, 1.0);
    teardown(&fixture);
}

int main(void)
{
    test_minimum_pressure_api();
    test_expression_y_api();
    test_blocks_and_reuse();
    test_quiet_block();
    test_overflow_isolation();
    test_sum_before_clip_and_rate();
    test_channel_pressure_api();
    test_key_pressure_api();
    test_api_queue_failure();
    test_excess_routes_remain_immediate();
    return EXIT_SUCCESS;
}
