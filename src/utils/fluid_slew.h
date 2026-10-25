#ifndef FLUID_SLEW_H
#define FLUID_SLEW_H

typedef struct
{
    double current;
    double target;
    double ramp_target, ramp_step, ramp_full, ramp_min;
} fluid_slew_t;

static inline void
fluid_slew_init(fluid_slew_t *slew, double value)
{
    slew->current = value;
    slew->target = value;
    slew->ramp_target = value;
    slew->ramp_step = slew->ramp_full = slew->ramp_min = 0.0;
}

static inline void
fluid_slew_set_target(fluid_slew_t *slew, double value)
{
    slew->target = value;
}

/* full_range_samples <= 0 means immediate.
 * elapsed_samples provides number of rendered samples since last advance (normally one internal rendering block)
 */


static inline double
fluid_slew_advance(fluid_slew_t *slew,
                   unsigned int elapsed_samples,
                   double full_range_samples)
{
    double delta;
    double step;

    if(full_range_samples <= 0.0)
    {
        slew->current = slew->target;
        return slew->current;
    }

    if(elapsed_samples == 0)
    {
        return slew->current;
    }

    delta = slew->target - slew->current;
    step = elapsed_samples / full_range_samples;

    if(delta > step)
    {
        slew->current += step;
    }
    else if(delta < -step)
    {
        slew->current -= step;
    }
    else
    {
        slew->current = slew->target;
    }

    return slew->current;
}

/* Linear ramp with a minimum duration, retargeted from the current value.
 * Cache speed at retarget time: recomputing distance every block would turn
 * the minimum-time portion into an exponential approach instead of a ramp.
 * Duplicate target messages must not restart a ramp.
 */
static inline double
fluid_slew_advance_min(fluid_slew_t *slew, unsigned int elapsed_samples,
                       double full_range_samples, double minimum_samples)
{
    double delta, distance, duration, step;
    if(full_range_samples <= 0.0 || minimum_samples <= 0.0)
    {
        slew->ramp_full = -1.0;
        return fluid_slew_advance(slew, elapsed_samples, full_range_samples);
    }
    if(minimum_samples > full_range_samples) minimum_samples = full_range_samples;
    delta = slew->target - slew->current;
    if(slew->ramp_target != slew->target || slew->ramp_full != full_range_samples ||
       slew->ramp_min != minimum_samples)
    {
        distance = delta < 0.0 ? -delta : delta;
        duration = distance * full_range_samples;
        if(duration < minimum_samples) duration = minimum_samples;
        slew->ramp_step = distance / duration;
        slew->ramp_target = slew->target;
        slew->ramp_full = full_range_samples;
        slew->ramp_min = minimum_samples;
    }
    step = elapsed_samples * slew->ramp_step;
    if(delta > step) slew->current += step;
    else if(delta < -step) slew->current -= step;
    else slew->current = slew->target;
    return slew->current;
}

#endif /* FLUID_SLEW_H */
