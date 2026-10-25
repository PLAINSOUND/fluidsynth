/* FluidSynth - A Software Synthesizer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#include "fluid_rvoice_mod.h"
#include "fluid_rvoice_event.h"
#include <math.h>

void
fluid_rvoice_mod_advance(fluid_rvoice_mod_t *record, unsigned int elapsed_samples)
{
    int i;

    if(!record->configured)
    {
        return;
    }

    for(i = 0; i < 2; i++)
    {
        if(record->source[i].slew_enabled)
        {
            fluid_slew_advance_min(&record->source[i].slew, elapsed_samples,
                               fluid_rvoice_mod_source_time(record, i), record->minimum_samples);
        }
    }
}

fluid_real_t
fluid_rvoice_mod_evaluate(const fluid_rvoice_mod_t *record)
{
    fluid_mod_t definition;
    fluid_real_t value[2];
    int i;

    if(!record->configured)
    {
        return 0.0;
    }

    for(i = 0; i < 2; i++)
    {
        /* Slew uses endpoint distance 127, while the mapping range remains
         * 128. Immediate sources use their latest raw input unchanged.
         */
        value[i] = record->source[i].slew_enabled
                   ? record->source[i].slew.current * 127.0
                   : record->source[i].value;
    }

    /* The existing evaluator accepts a mutable modulator because custom
     * callbacks can modify it. We exclude those mappings and pass a local
     * scalar-only copy, keeping evaluation itself free of state changes.
     * Supported attenuation routes do not need a synthesis-side voice.
     */
    definition = record->definition;
    return fluid_mod_get_value_with_sources(&definition, NULL,
                                            value[0], record->source[0].range,
                                            value[1], record->source[1].range);
}

/* Match fluid_mod_get_source_value(): these are mapping ranges, not the
 * endpoint distance (127) used to normalize pressure slew state.
 */
static fluid_real_t
fluid_rvoice_mod_source_range(unsigned char source, unsigned char flags)
{
    return !(flags & FLUID_MOD_CC) && source == FLUID_MOD_PITCHWHEEL
           ? 16384.0 : 128.0;
}

static int
fluid_rvoice_mod_input_valid(unsigned char source, unsigned char flags,
                            fluid_real_t value)
{
    fluid_real_t range = fluid_rvoice_mod_source_range(source, flags);

    if(!isfinite(value))
    {
        return 0;
    }

    /* NONE conventionally supplies range, rather than a MIDI controller. */
    if(!(flags & FLUID_MOD_CC) && source == FLUID_MOD_NONE)
    {
        return value == range;
    }

    return value >= 0.0 && value <= range - 1.0;
}

/* Consumer-side handler: called through dispatch_all(), never directly by
 * the producer. Reconstruct a local scalar definition, then initialize the
 * render-owned record. No pointer to the producer's definition is retained.
 */
static DECLARE_FLUID_RVOICE_FUNCTION(fluid_rvoice_mod_config_event)
{
    fluid_rvoice_mod_t *record = obj;
    fluid_mod_t definition = {0};

    definition.dest = param[0].i & 0xff;
    definition.src1 = (param[0].i >> 8) & 0xff;
    definition.flags1 = (param[0].i >> 16) & 0xff;
    definition.src2 = param[1].i & 0xff;
    definition.flags2 = (param[1].i >> 8) & 0xff;
    definition.trans = (param[1].i >> 16) & 0xff;
    definition.amount = param[2].real;

    fluid_rvoice_mod_init_times(record, &definition,
                         param[3].real,
                         fluid_rvoice_mod_source_range(definition.src1, definition.flags1),
                         param[4].real,
                         fluid_rvoice_mod_source_range(definition.src2, definition.flags2),
                         param[5].real, param[6].real);
}

static DECLARE_FLUID_RVOICE_FUNCTION(fluid_rvoice_mod_inputs_event)
{
    fluid_rvoice_mod_t *record = obj;
    int i;

    if(!record->configured ||
       !fluid_rvoice_mod_input_valid(record->definition.src1,
                                    record->definition.flags1, param[0].real) ||
       !fluid_rvoice_mod_input_valid(record->definition.src2,
                                    record->definition.flags2, param[1].real))
    {
        return;
    }

    for(i = 0; i < 2; i++)
    {
        record->source[i].value = param[i].real;

        if(record->source[i].slew_enabled)
        {
            fluid_slew_set_target(&record->source[i].slew, param[i].real / 127.0);

            if(fluid_rvoice_mod_source_time(record, i) <= 0.0)
            {
                fluid_slew_advance(&record->source[i].slew, 0, 0.0);
            }
        }
    }
}

static DECLARE_FLUID_RVOICE_FUNCTION(fluid_rvoice_mod_minimum_event)
{
    fluid_rvoice_mod_t *record = obj;
    record->minimum_samples = param[0].real;
}

int fluid_rvoice_mod_enqueue_minimum(fluid_rvoice_eventhandler_t *handler,
    fluid_rvoice_mod_t *record, fluid_real_t samples)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};
    if(!isfinite(samples) || samples < 0.0) return FLUID_FAILED;
    param[0].real = samples;
    return fluid_rvoice_eventhandler_push(handler, fluid_rvoice_mod_minimum_event, record, param);
}

static DECLARE_FLUID_RVOICE_FUNCTION(fluid_rvoice_mod_clear_event)
{
    (void)param;
    FLUID_MEMSET(obj, 0, sizeof(fluid_rvoice_mod_t));
}

static DECLARE_FLUID_RVOICE_FUNCTION(fluid_rvoice_mod_duration_event)
{
    fluid_rvoice_mod_t *record = obj;
    int i;
    if(!record->configured)
    {
        return;
    }
    record->full_range_samples = param[0].real;
    record->expression_full_range_samples = param[1].real;
    for(i = 0; i < 2; i++)
    {
        if(record->source[i].slew_enabled && fluid_rvoice_mod_source_time(record, i) == 0.0)
        {
            fluid_slew_advance(&record->source[i].slew, 0, 0.0);
        }
    }
}

int
fluid_rvoice_mod_enqueue_duration(fluid_rvoice_eventhandler_t *handler,
                                 fluid_rvoice_mod_t *record,
                                 fluid_real_t full_range_samples)
{
    return fluid_rvoice_mod_enqueue_times(handler, record, full_range_samples, 0.0);
}

int
fluid_rvoice_mod_enqueue_times(fluid_rvoice_eventhandler_t *handler,
                             fluid_rvoice_mod_t *record,
                             fluid_real_t full_range_samples, fluid_real_t expression_samples)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};
    if(!isfinite(full_range_samples) || full_range_samples < 0.0 ||
       !isfinite(expression_samples) || expression_samples < 0.0)
    {
        return FLUID_FAILED;
    }
    param[0].real = full_range_samples;
    param[1].real = expression_samples;
    return fluid_rvoice_eventhandler_push(handler, fluid_rvoice_mod_duration_event,
                                         record, param);
}

int
fluid_rvoice_mod_config_valid(const fluid_mod_t *definition,
                             fluid_real_t value1, fluid_real_t value2,
                             fluid_real_t full_range_samples)
{
    return fluid_rvoice_mod_config_valid_times(definition, value1, value2, full_range_samples, 0.0);
}

int
fluid_rvoice_mod_config_valid_times(const fluid_mod_t *definition,
                             fluid_real_t value1, fluid_real_t value2,
                             fluid_real_t full_range_samples, fluid_real_t expression_samples)
{
    fluid_rvoice_mod_t checked;

    /* Validate using producer-local storage; never inspect the live record. */
    if(!isfinite(full_range_samples) || full_range_samples < 0.0 ||
       !isfinite(expression_samples) || expression_samples < 0.0 ||
       !isfinite(definition->amount) ||
       !fluid_rvoice_mod_input_valid(definition->src1, definition->flags1, value1) ||
       !fluid_rvoice_mod_input_valid(definition->src2, definition->flags2, value2) ||
       fluid_rvoice_mod_init_times(&checked, definition, value1,
                            fluid_rvoice_mod_source_range(definition->src1, definition->flags1),
                            value2,
                            fluid_rvoice_mod_source_range(definition->src2, definition->flags2),
                            full_range_samples, expression_samples) != FLUID_OK)
    {
        return 0;
    }
    return 1;
}

int
fluid_rvoice_mod_enqueue_config(fluid_rvoice_eventhandler_t *handler,
                               fluid_rvoice_mod_t *record,
                               const fluid_mod_t *definition,
                               fluid_real_t value1, fluid_real_t value2,
                               fluid_real_t full_range_samples)
{
    return fluid_rvoice_mod_enqueue_config_times(handler, record, definition,
                                                value1, value2, full_range_samples, 0.0);
}

int
fluid_rvoice_mod_enqueue_config_times(fluid_rvoice_eventhandler_t *handler,
                               fluid_rvoice_mod_t *record,
                               const fluid_mod_t *definition,
                               fluid_real_t value1, fluid_real_t value2,
                               fluid_real_t full_range_samples, fluid_real_t expression_samples)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};
    if(!fluid_rvoice_mod_config_valid_times(definition, value1, value2,
                                            full_range_samples, expression_samples))
    {
        return FLUID_FAILED;
    }

    /* Each packed integer uses just 24 bits, remaining positive in int32.
     * One event carries the whole configuration: no partial configuration
     * can be published if the queue is full. Unused parameters stay zero.
     */
    param[0].i = definition->dest | (definition->src1 << 8) | (definition->flags1 << 16);
    param[1].i = definition->src2 | (definition->flags2 << 8) | (definition->trans << 16);
    param[2].real = definition->amount;
    param[3].real = value1;
    param[4].real = value2;
    param[5].real = full_range_samples;
    param[6].real = expression_samples;

    return fluid_rvoice_eventhandler_push(handler, fluid_rvoice_mod_config_event,
                                         record, param);
}

int
fluid_rvoice_mod_enqueue_inputs(fluid_rvoice_eventhandler_t *handler,
                               fluid_rvoice_mod_t *record,
                               fluid_real_t value1, fluid_real_t value2)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};

    /* Source-specific validation is consumer-side: the producer cannot read
     * record->definition, which may still be waiting to be configured.
     */
    if(!isfinite(value1) || !isfinite(value2))
    {
        return FLUID_FAILED;
    }

    param[0].real = value1;
    param[1].real = value2;
    return fluid_rvoice_eventhandler_push(handler, fluid_rvoice_mod_inputs_event,
                                         record, param);
}

int
fluid_rvoice_mod_enqueue_clear(fluid_rvoice_eventhandler_t *handler,
                              fluid_rvoice_mod_t *record)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {0};
    return fluid_rvoice_eventhandler_push(handler, fluid_rvoice_mod_clear_event,
                                         record, param);
}
