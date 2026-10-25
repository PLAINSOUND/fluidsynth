#include "test.h"
#include "fluidsynth.h"
#include <math.h>

static const char *setting_name = "synth.pressure-attenuation-slew-ms";

static void assert_value(fluid_settings_t *settings, double expected)
{
    double value;
    TEST_SUCCESS(fluid_settings_getnum(settings, setting_name, &value));
    TEST_ASSERT(fabs(value - expected) < 1e-10);
}

int main(void)
{
    fluid_settings_t *settings = new_fluid_settings();
    fluid_settings_t *independent = new_fluid_settings();
    double minimum, maximum, default_value;

    TEST_ASSERT(settings != NULL && independent != NULL);
    TEST_ASSERT(fluid_settings_get_type(settings, setting_name) == FLUID_NUM_TYPE);
    TEST_SUCCESS(fluid_settings_getnum_default(settings, setting_name, &default_value));
    TEST_ASSERT(default_value == 0.0);
    assert_value(settings, 0.0);

    TEST_SUCCESS(fluid_settings_getnum_range(settings, setting_name, &minimum, &maximum));
    TEST_ASSERT(minimum == 0.0 && maximum == 500.0);

    /* Fractional milliseconds must not be rounded to an integer setting. */
    TEST_SUCCESS(fluid_settings_setnum(settings, setting_name, 12.75));
    assert_value(settings, 12.75);
    assert_value(independent, 0.0);
    TEST_SUCCESS(fluid_settings_getnum_default(settings, setting_name, &default_value));
    TEST_ASSERT(default_value == 0.0);

    /* Ordinary settings bounds reject invalid values without changing state. */
    TEST_ASSERT(fluid_settings_setnum(settings, setting_name, -0.01) == FLUID_FAILED);
    assert_value(settings, 12.75);
    TEST_ASSERT(fluid_settings_setnum(settings, setting_name, 500.01) == FLUID_FAILED);
    assert_value(settings, 12.75);

    TEST_SUCCESS(fluid_settings_setnum(settings, setting_name, maximum));
    assert_value(settings, 500.0);
    TEST_SUCCESS(fluid_settings_setnum(settings, setting_name, 0.0));
    assert_value(settings, 0.0);

    /* Realtime callbacks are installed by the synth, not by settings alone. */
    TEST_ASSERT(!fluid_settings_is_realtime(settings, setting_name));
    {
        fluid_synth_t *synth = new_fluid_synth(settings);
        TEST_ASSERT(synth != NULL);
        TEST_ASSERT(fluid_settings_is_realtime(settings, setting_name));
        TEST_SUCCESS(fluid_settings_setnum(settings, setting_name, 25.0));
        delete_fluid_synth(synth);
        TEST_ASSERT(!fluid_settings_is_realtime(settings, setting_name));
        TEST_SUCCESS(fluid_settings_setnum(settings, setting_name, 0.0));
    }

    delete_fluid_settings(independent);
    delete_fluid_settings(settings);
    return EXIT_SUCCESS;
}
