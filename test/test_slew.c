#include "test.h"
#include "fluid_slew.h"
#include <math.h>

static void assert_close(double actual, double expected)
{
    TEST_ASSERT(fabs(actual - expected) < 1e-10);
}

int main(void)
{
    fluid_slew_t slew;

    /* Full-range movement: 128 samples. */
    fluid_slew_init(&slew, 0.0);
    fluid_slew_set_target(&slew, 1.0);
    assert_close(fluid_slew_advance(&slew, 64, 128.0), 0.5);
    assert_close(fluid_slew_advance(&slew, 64, 128.0), 1.0);

    /* Ten MIDI units: 10 samples with a full-range duration of 127.0 samples. */
    fluid_slew_init(&slew, 0.0);
    fluid_slew_set_target(&slew, 10.0 / 127.0);
    assert_close(fluid_slew_advance(&slew, 5, 127.0),
                 5.0 / 127.0);
    assert_close(fluid_slew_advance(&slew, 5, 127.0),
                 10.0 / 127.0);

    /* Retargeting preserves the current value and reverses direction. */
    fluid_slew_init(&slew, 0.0);
    fluid_slew_set_target(&slew, 1.0);
    fluid_slew_advance(&slew, 25, 100.0);
    fluid_slew_set_target(&slew, 0.0);
    assert_close(slew.current, 0.25);
    assert_close(fluid_slew_advance(&slew, 10, 100.0), 0.15);
    assert_close(fluid_slew_advance(&slew, 100, 100.0), 0.0);

    /* Disabling smoothing adopts the target immediately. */
    fluid_slew_set_target(&slew, 0.8);
    assert_close(fluid_slew_advance(&slew, 0, 0.0), 0.8);

    /* Zero elapsed samples leave the value unchanged when smoothing is enabled. */
    fluid_slew_init(&slew, 0.0);
    fluid_slew_set_target(&slew, 0.8);
    assert_close(fluid_slew_advance(&slew, 0, 127.0), 0.0);

    /* Initializing a new voice does not create a ramp. */
    fluid_slew_init(&slew, 0.6);
    assert_close(fluid_slew_advance(&slew, 1, 100.0), 0.6);

    /* Small changes take the minimum, without an exponential tail. */
    fluid_slew_init(&slew, 0.0);
    fluid_slew_set_target(&slew, 0.1);
    assert_close(fluid_slew_advance_min(&slew, 10, 100, 40), 0.025);
    fluid_slew_set_target(&slew, 0.1); /* Duplicate input must not restart. */
    assert_close(fluid_slew_advance_min(&slew, 10, 100, 40), 0.05);
    assert_close(fluid_slew_advance_min(&slew, 20, 100, 40), 0.1);
    /* Minimum == full range gives the same duration for every distance. */
    fluid_slew_set_target(&slew, 0.5);
    assert_close(fluid_slew_advance_min(&slew, 50, 100, 100), 0.3);
    assert_close(fluid_slew_advance_min(&slew, 50, 100, 100), 0.5);
    /* Reverse from the current position; zero smoothing always bypasses. */
    fluid_slew_set_target(&slew, 0.0);
    assert_close(fluid_slew_advance_min(&slew, 20, 100, 100), 0.4);
    assert_close(fluid_slew_advance_min(&slew, 0, 0, 100), 0.0);
    return 0;
}
