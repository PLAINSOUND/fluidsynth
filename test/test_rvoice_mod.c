#include "test.h"
#include "fluid_rvoice_mod.h"
#include "fluid_rvoice_event.h"
#include <math.h>
#include <float.h>

/* Do not rely on C99 NAN/INFINITY macros in Solaris' C90 mode. */
static double test_nonfinite(int nan)
{
    uint64_t bits = (uint64_t)0x7ff00000 << 32;
    double value;
    if(nan) bits |= (uint64_t)0x00080000 << 32;
    FLUID_MEMCPY(&value, &bits, sizeof(value));
    return value;
}

static void test_finite_classification(void)
{
    TEST_ASSERT(fluid_isfinite(0.0));
    TEST_ASSERT(fluid_isfinite(-0.0));
    TEST_ASSERT(fluid_isfinite(DBL_MAX));
    TEST_ASSERT(fluid_isfinite(-DBL_MAX));
    TEST_ASSERT(fluid_isfinite(DBL_MIN));
    TEST_ASSERT(fluid_isfinite(DBL_MIN / 2.0));
    TEST_ASSERT(!fluid_isfinite(test_nonfinite(0)));
    TEST_ASSERT(!fluid_isfinite(-test_nonfinite(0)));
    TEST_ASSERT(!fluid_isfinite(test_nonfinite(1)));
}

static void test_snapshot(void)
{
    fluid_mod_t original = {0};
    fluid_mod_t next = {0};
    fluid_rvoice_mod_t record;
    int callback_data = 42;

    original.dest = GEN_ATTENUATION;
    original.src1 = FLUID_MOD_CHANNELPRESSURE;
    original.flags1 = FLUID_MOD_NEGATIVE | FLUID_MOD_CONCAVE;
    original.src2 = FLUID_MOD_NONE;
    original.amount = 960.0;
    original.data = &callback_data;
    original.next = &next;

    TEST_SUCCESS(fluid_rvoice_mod_init(&record, &original,
                                      64.0, 128.0, 128.0, 128.0, 4800.0));
    TEST_ASSERT(record.definition.amount == 960.0);
    TEST_ASSERT(record.definition.flags1 == original.flags1);
    TEST_ASSERT(record.definition.mapping_func == NULL);
    TEST_ASSERT(record.definition.data == NULL);
    TEST_ASSERT(record.definition.next == NULL);
    TEST_ASSERT(record.source[0].slew_enabled);
    TEST_ASSERT(!record.source[1].slew_enabled);
    TEST_ASSERT(FLUID_FABS(record.source[0].slew.current - 64.0 / 127.0) < 0.000001);
    TEST_ASSERT(record.source[0].slew.current == record.source[0].slew.target);
    TEST_ASSERT(record.source[0].range == 128.0);
    TEST_ASSERT(record.full_range_samples == 4800.0);

    original.amount = 100.0;
    TEST_ASSERT(record.definition.amount == 960.0);

    /* Reinitialization replaces old progress, as voice reuse will require. */
    TEST_SUCCESS(fluid_rvoice_mod_init(&record, &original,
                                      0.0, 128.0, 128.0, 128.0, 0.0));
    TEST_ASSERT(record.source[0].slew.current == 0.0);
    TEST_ASSERT(record.source[0].slew.target == 0.0);
}

static void test_selection(void)
{
    fluid_mod_t mod = {0};
    fluid_rvoice_mod_t record;

    mod.dest = GEN_ATTENUATION;
    mod.src1 = FLUID_MOD_NONE;
    mod.src2 = FLUID_MOD_KEYPRESSURE;
    mod.amount = 100.0;

    TEST_SUCCESS(fluid_rvoice_mod_init(&record, &mod,
                                      128.0, 128.0, 127.0, 128.0, 4800.0));
    TEST_ASSERT(!record.source[0].slew_enabled);
    TEST_ASSERT(record.source[1].slew_enabled);
    TEST_ASSERT(record.source[1].slew.current == 1.0);

    mod.src1 = FLUID_MOD_CHANNELPRESSURE;
    TEST_SUCCESS(fluid_rvoice_mod_init(&record, &mod,
                                      64.0, 128.0, 127.0, 128.0, 4800.0));
    TEST_ASSERT(record.source[0].slew_enabled && record.source[1].slew_enabled);

    /* A CC with the same numeric ID is not general-controller pressure. */
    mod.src1 = FLUID_MOD_NONE;
    mod.flags2 = FLUID_MOD_CC;
    TEST_ASSERT(fluid_rvoice_mod_init(&record, &mod,
                                    128.0, 128.0, 64.0, 128.0, 4800.0) == FLUID_FAILED);

    mod.flags2 = FLUID_MOD_CUSTOM;
    TEST_ASSERT(fluid_rvoice_mod_init(&record, &mod,
                                    128.0, 128.0, 64.0, 128.0, 4800.0) == FLUID_FAILED);

    mod.flags2 = FLUID_MOD_SWITCH;
    TEST_ASSERT(fluid_rvoice_mod_init(&record, &mod,
                                    128.0, 128.0, 64.0, 128.0, 4800.0) == FLUID_FAILED);

    mod.flags2 = FLUID_MOD_BIPOLAR;
    TEST_ASSERT(fluid_rvoice_mod_init(&record, &mod,
                                    128.0, 128.0, 64.0, 128.0, 4800.0) == FLUID_FAILED);

    mod.flags2 = FLUID_MOD_LINEAR;
    mod.dest = GEN_FILTERFC;
    TEST_ASSERT(fluid_rvoice_mod_init(&record, &mod,
                                    128.0, 128.0, 64.0, 128.0, 4800.0) == FLUID_FAILED);
}

/* Exercise the real event queue/dispatcher without allocating an audio mixer.
 * These tests intentionally run producer and consumer phases sequentially;
 * they test the protocol, not concurrent stress or audio integration.
 */
static void test_handoff(void)
{
    fluid_rvoice_eventhandler_t handler = {0};
    fluid_rvoice_mod_t record = {0};
    fluid_mod_t definition = {0};
    int i;

    handler.queue = new_fluid_ringbuffer(3, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler.queue != NULL);

    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_CHANNELPRESSURE;
    definition.flags1 = FLUID_MOD_NEGATIVE | FLUID_MOD_CONCAVE;
    definition.src2 = 11;
    definition.flags2 = FLUID_MOD_CC | FLUID_MOD_LINEAR;
    definition.amount = 960.0;
    definition.trans = FLUID_MOD_TRANSFORM_ABS;

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                64.0, 100.0, 4800.25));
    TEST_ASSERT(!record.configured);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 0);

    /* The queue owns copies of the scalar payload, not this definition. */
    definition.amount = 1.0;
    definition.src1 = FLUID_MOD_NONE;
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(!record.configured);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    TEST_ASSERT(record.configured);
    TEST_ASSERT(record.definition.amount == 960.0);
    TEST_ASSERT(record.definition.src1 == FLUID_MOD_CHANNELPRESSURE);
    TEST_ASSERT(record.definition.flags1 == (FLUID_MOD_NEGATIVE | FLUID_MOD_CONCAVE));
    TEST_ASSERT(record.definition.src2 == 11);
    TEST_ASSERT(record.definition.flags2 == (FLUID_MOD_CC | FLUID_MOD_LINEAR));
    TEST_ASSERT(record.definition.trans == FLUID_MOD_TRANSFORM_ABS);
    TEST_ASSERT(record.definition.data == NULL && record.definition.next == NULL);
    TEST_ASSERT(record.full_range_samples == 4800.25);

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 80.0, 90.0));
    TEST_ASSERT(record.source[0].value == 64.0);
    TEST_ASSERT(record.source[1].value == 100.0);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 0);
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    TEST_ASSERT(record.source[0].value == 80.0);
    TEST_ASSERT(FLUID_FABS(record.source[0].slew.target - 80.0 / 127.0) < 0.000001);
    TEST_ASSERT(FLUID_FABS(record.source[0].slew.current - 64.0 / 127.0) < 0.000001);
    TEST_ASSERT(record.source[1].value == 90.0);
    TEST_ASSERT(!record.source[1].slew_enabled);

    /* FIFO ordering: clear runs first, so a later input cannot revive it. */
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_clear(&handler, &record));
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 100.0, 90.0));
    TEST_ASSERT(record.configured);
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 2);
    TEST_ASSERT(!record.configured);
    TEST_ASSERT(record.source[0].slew.target == 0.0);

    /* Reconfigure with pressure in source 2 and zero slew duration. */
    definition.src1 = FLUID_MOD_NONE;
    definition.flags1 = 0;
    definition.src2 = FLUID_MOD_KEYPRESSURE;
    definition.flags2 = FLUID_MOD_LINEAR;
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                128.0, 20.0, 0.0));
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 128.0, 40.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 2);
    TEST_ASSERT(record.configured && record.source[1].slew_enabled);
    TEST_ASSERT(FLUID_FABS(record.source[1].slew.current - 40.0 / 127.0) < 0.000001);

    /* Repeated round trips also exercise ring-buffer wraparound. */
    for(i = 0; i < 10; i++)
    {
        TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 128.0, i));
        fluid_rvoice_eventhandler_flush(&handler);
        TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
        TEST_ASSERT(record.source[1].value == i);
    }

    /* A numerically invalid update is ignored as a whole by the consumer. */
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 128.0, 128.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    TEST_ASSERT(record.source[1].value == 9.0);

    delete_fluid_ringbuffer(handler.queue);
}

static void test_full_queue(void)
{
    fluid_rvoice_eventhandler_t handler = {0};
    fluid_rvoice_mod_t record = {0};
    fluid_mod_t definition = {0};

    handler.queue = new_fluid_ringbuffer(1, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler.queue != NULL);
    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_KEYPRESSURE;
    definition.amount = 100.0;

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                64.0, 128.0, 4800.0));
    TEST_ASSERT(fluid_rvoice_mod_enqueue_clear(&handler, &record) == FLUID_FAILED);
    TEST_ASSERT(fluid_atomic_int_get(&handler.queue_stored) == 1);
    TEST_ASSERT(!record.configured);
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    TEST_ASSERT(record.configured);

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 80.0, 128.0));
    definition.amount = 200.0;
    TEST_ASSERT(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                               0.0, 128.0, 0.0) == FLUID_FAILED);
    TEST_ASSERT(record.definition.amount == 100.0);
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    TEST_ASSERT(record.definition.amount == 100.0);
    TEST_ASSERT(record.source[0].value == 80.0);
    TEST_ASSERT(FLUID_FABS(record.source[0].slew.current - 64.0 / 127.0) < 0.000001);

    delete_fluid_ringbuffer(handler.queue);
}

static void test_invalid_handoff(void)
{
    fluid_rvoice_eventhandler_t handler = {0};
    fluid_rvoice_mod_t record = {0};
    fluid_mod_t definition = {0};

    handler.queue = new_fluid_ringbuffer(2, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler.queue != NULL);
    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_CHANNELPRESSURE;
    TEST_ASSERT(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                               128.0, 128.0, 4800.0) == FLUID_FAILED);
    TEST_ASSERT(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                               64.0, 128.0, -1.0) == FLUID_FAILED);
    TEST_ASSERT(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                               64.0, 128.0, test_nonfinite(0)) == FLUID_FAILED);
    TEST_ASSERT(fluid_rvoice_mod_enqueue_inputs(&handler, &record,
                                               test_nonfinite(1), 128.0) == FLUID_FAILED);
    definition.flags1 = FLUID_MOD_CUSTOM;
    TEST_ASSERT(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                               64.0, 128.0, 4800.0) == FLUID_FAILED);
    TEST_ASSERT(fluid_atomic_int_get(&handler.queue_stored) == 0);
    TEST_ASSERT(!record.configured);
    delete_fluid_ringbuffer(handler.queue);
}

static void assert_value(fluid_real_t actual, fluid_real_t expected)
{
    TEST_ASSERT(FLUID_FABS(actual - expected) < 0.00001);
}

static void test_advance_and_evaluate(void)
{
    fluid_rvoice_eventhandler_t handler = {0};
    fluid_rvoice_mod_t record = {0};
    fluid_rvoice_mod_t partitioned;
    fluid_rvoice_mod_t small_step;
    fluid_mod_t definition = {0};
    int i;

    handler.queue = new_fluid_ringbuffer(2, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler.queue != NULL);
    assert_value(fluid_rvoice_mod_evaluate(&record), 0.0);
    fluid_rvoice_mod_advance(&record, 64);
    TEST_ASSERT(!record.configured);

    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_CHANNELPRESSURE;
    definition.amount = 128.0;
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                0.0, 128.0, 256.0));
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 127.0, 128.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 2);
    partitioned = record;

    assert_value(fluid_rvoice_mod_evaluate(&record), 0.0);
    fluid_rvoice_mod_advance(&record, 0);
    assert_value(fluid_rvoice_mod_evaluate(&record), 0.0);

    for(i = 1; i <= 4; i++)
    {
        fluid_rvoice_mod_advance(&record, 64);
        assert_value(fluid_rvoice_mod_evaluate(&record), 127.0 * i / 4.0);
        /* Repeated evaluation must not move the ramp again. */
        assert_value(fluid_rvoice_mod_evaluate(&record), 127.0 * i / 4.0);
    }

    for(i = 0; i < 16; i++)
    {
        fluid_rvoice_mod_advance(&partitioned, 16);
    }
    assert_value(fluid_rvoice_mod_evaluate(&partitioned),
                 fluid_rvoice_mod_evaluate(&record));
    fluid_rvoice_mod_advance(&record, 1024);
    assert_value(fluid_rvoice_mod_evaluate(&record), 127.0);

    /* A ten-unit delta takes 10/127 of the full-range sample duration. */
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                0.0, 128.0, 1270.0));
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 10.0, 128.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 2);
    small_step = record;
    fluid_rvoice_mod_advance(&small_step, 100);
    assert_value(fluid_rvoice_mod_evaluate(&small_step), 10.0);
    fluid_rvoice_mod_advance(&record, 50);
    assert_value(fluid_rvoice_mod_evaluate(&record), 5.0);

    /* Reverse midway, without jumping to either old or new target. */
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 0.0, 128.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    assert_value(fluid_rvoice_mod_evaluate(&record), 5.0);
    fluid_rvoice_mod_advance(&record, 20);
    assert_value(fluid_rvoice_mod_evaluate(&record), 3.0);
    fluid_rvoice_mod_advance(&record, 30);
    assert_value(fluid_rvoice_mod_evaluate(&record), 0.0);

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_clear(&handler, &record));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    fluid_rvoice_mod_advance(&record, 64);
    assert_value(fluid_rvoice_mod_evaluate(&record), 0.0);
    /* An independent record has not been cleared or retargeted. */
    assert_value(fluid_rvoice_mod_evaluate(&partitioned), 127.0);
    delete_fluid_ringbuffer(handler.queue);
}

static void test_immediate_secondary_input(void)
{
    fluid_rvoice_eventhandler_t handler = {0};
    fluid_rvoice_mod_t record = {0};
    fluid_mod_t definition = {0};

    handler.queue = new_fluid_ringbuffer(2, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(handler.queue != NULL);
    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_KEYPRESSURE;
    definition.src2 = 11;
    definition.flags2 = FLUID_MOD_CC;
    definition.amount = 128.0;
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&handler, &record, &definition,
                                                0.0, 64.0, 256.0));
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 127.0, 64.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 2);
    fluid_rvoice_mod_advance(&record, 64);
    assert_value(fluid_rvoice_mod_evaluate(&record), 31.75 * 0.5);

    TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&handler, &record, 127.0, 32.0));
    fluid_rvoice_eventhandler_flush(&handler);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_all(&handler) == 1);
    /* CC11 changes the result immediately; pressure has not advanced. */
    assert_value(fluid_rvoice_mod_evaluate(&record), 31.75 * 0.25);
    fluid_rvoice_mod_advance(&record, 64);
    assert_value(fluid_rvoice_mod_evaluate(&record), 63.5 * 0.25);
    delete_fluid_ringbuffer(handler.queue);
}

static void test_both_sources_and_curves(void)
{
    const int curves[] = {FLUID_MOD_LINEAR, FLUID_MOD_CONCAVE, FLUID_MOD_CONVEX};
    fluid_rvoice_mod_t record;
    fluid_mod_t definition = {0};
    int i;

    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_CHANNELPRESSURE;
    definition.src2 = FLUID_MOD_KEYPRESSURE;
    definition.amount = 128.0;

    for(i = 0; i < 3; i++)
    {
        definition.flags1 = curves[i] | FLUID_MOD_NEGATIVE;
        definition.flags2 = curves[i];
        TEST_SUCCESS(fluid_rvoice_mod_init(&record, &definition,
                                          64.0, 128.0, 32.0, 128.0, 256.0));
        /* Test-owned state: no other thread can access this local record. */
        fluid_slew_set_target(&record.source[0].slew, 127.0 / 127.0);
        fluid_slew_set_target(&record.source[1].slew, 0.0);
        fluid_rvoice_mod_advance(&record, 32);
        assert_value(fluid_rvoice_mod_evaluate(&record),
                     fluid_mod_get_value_with_sources(&definition, NULL,
                         79.875, 128.0, 16.125, 128.0));
    }

    /* Zero duration bypasses the ramp even with zero elapsed samples. */
    definition.flags1 = definition.flags2 = FLUID_MOD_LINEAR;
    TEST_SUCCESS(fluid_rvoice_mod_init(&record, &definition,
                                      64.0, 128.0, 32.0, 128.0, 0.0));
    fluid_slew_set_target(&record.source[0].slew, 1.0);
    fluid_rvoice_mod_advance(&record, 0);
    assert_value(fluid_rvoice_mod_evaluate(&record), 127.0 * 32.0 / 128.0);
}

static void test_expression_times(void)
{
    fluid_mod_t mod = {0};
    fluid_rvoice_mod_t record;
    int cc;
    mod.dest = GEN_ATTENUATION;
    mod.src1 = FLUID_MOD_CHANNELPRESSURE;
    mod.src2 = 74;
    mod.flags2 = FLUID_MOD_CC;
    mod.amount = 128.0;
    for(cc = 1; cc <= 74; cc += 73)
    {
        mod.src2 = cc;
        TEST_SUCCESS(fluid_rvoice_mod_init_times(&record, &mod,
            0.0, 128.0, 0.0, 128.0, 1000.0, 500.0));
        fluid_slew_set_target(&record.source[0].slew, 1.0);
        fluid_slew_set_target(&record.source[1].slew, 1.0);
        fluid_rvoice_mod_advance(&record, 100);
        assert_value(record.source[0].slew.current, 0.1);
        assert_value(record.source[1].slew.current, 0.2);
        assert_value(fluid_rvoice_mod_evaluate(&record), 12.7 * 25.4 / 128.0);
        record.expression_full_range_samples = 0;
        fluid_rvoice_mod_advance(&record, 0);
        assert_value(record.source[0].slew.current, 0.1);
        assert_value(record.source[1].slew.current, 1.0);
    }
}

int main(void)
{
    test_finite_classification();
    test_expression_times();
    test_snapshot();
    test_selection();
    test_handoff();
    test_full_queue();
    test_invalid_handoff();
    test_advance_and_evaluate();
    test_immediate_secondary_input();
    test_both_sources_and_curves();
    return EXIT_SUCCESS;
}
