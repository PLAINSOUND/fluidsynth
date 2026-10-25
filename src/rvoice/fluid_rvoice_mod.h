/* FluidSynth - A Software Synthesizer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 */

#ifndef FLUID_RVOICE_MOD_H
#define FLUID_RVOICE_MOD_H

#include "fluid_mod.h"
#include "fluid_slew.h"

/* One input of a render-owned modulator. Values and ranges use FluidSynth's
 * raw source units; pressure slew state uses the normalized range 0..1.
 */
typedef struct
{
    fluid_real_t value;
    fluid_real_t range;
    fluid_slew_t slew;
    int slew_enabled;
} fluid_rvoice_mod_source_t;

/* One selected modulation route, not the final destination subtotal.
 * definition contains a scalar-only snapshot: mapping_func, data and next
 * remain NULL. The record must only be changed by its rendering owner, or
 * while exclusively owned by a test. No live voice/channel pointer is kept.
 */
typedef struct
{
    fluid_mod_t definition;
    fluid_rvoice_mod_source_t source[2];
    double full_range_samples;
    double expression_full_range_samples;
    double minimum_samples;
    int configured;
} fluid_rvoice_mod_t;

static inline int
fluid_rvoice_mod_is_expression_y(unsigned char source, unsigned char flags)
{
    return (flags & FLUID_MOD_CC) && (source == 1 || source == 74);
}

static inline double
fluid_rvoice_mod_source_time(const fluid_rvoice_mod_t *record, int i)
{
    return fluid_rvoice_mod_is_expression_y(
        i ? record->definition.src2 : record->definition.src1,
        i ? record->definition.flags2 : record->definition.flags1)
        ? record->expression_full_range_samples : record->full_range_samples;
}

static inline int
fluid_rvoice_mod_is_pressure(unsigned char source, unsigned char flags)
{
    return !(flags & FLUID_MOD_CC) &&
           (source == FLUID_MOD_KEYPRESSURE ||
            source == FLUID_MOD_CHANNELPRESSURE);
}

/* Initial scope: unipolar, standard continuous pressure/CC1/CC74 -> attenuation.
 * Check both inputs: even an immediate secondary input must be safe to
 * evaluate without a synthesis-side voice or a custom callback.
 *
 * Bipolar mappings are deferred, not inherently unsuitable for slewing.
 * Before enabling them, test midpoint/sign crossings and endpoint behaviour
 * for linear, concave and convex curves.
 *
 * Custom mappings require separate handling: their callbacks receive integer
 * inputs and may access external mutable state.
 * Switch mappings intentionally remain discontinuous.
 */
static inline int
fluid_rvoice_mod_source_supported(unsigned char source, unsigned char flags)
{
    int curve;

    if(!(flags & FLUID_MOD_CC) && source == FLUID_MOD_NONE)
    {
        return 1;
    }

    if(flags & FLUID_MOD_CUSTOM || flags & FLUID_MOD_BIPOLAR)
    {
        return 0;
    }

    curve = flags & FLUID_MOD_MAP_MASK;
    return curve == FLUID_MOD_LINEAR || curve == FLUID_MOD_CONCAVE ||
           curve == FLUID_MOD_CONVEX;
}

/* Inputs must be finite, valid raw controller values/ranges and a finite,
 * nonnegative duration. Validation at the API boundary will enforce this.
 * This copies values; it does NOT provide a cross-thread transfer mechanism.
 */
static inline int
fluid_rvoice_mod_init_times(fluid_rvoice_mod_t *record, const fluid_mod_t *definition,
                     fluid_real_t value1, fluid_real_t range1,
                     fluid_real_t value2, fluid_real_t range2,
                     double full_range_samples, double expression_samples)
{
    int pressure1 = fluid_rvoice_mod_is_pressure(definition->src1, definition->flags1) ||
                    fluid_rvoice_mod_is_expression_y(definition->src1, definition->flags1);
    int pressure2 = fluid_rvoice_mod_is_pressure(definition->src2, definition->flags2) ||
                    fluid_rvoice_mod_is_expression_y(definition->src2, definition->flags2);

    FLUID_MEMSET(record, 0, sizeof(*record));

    if(definition->dest != GEN_ATTENUATION || !(pressure1 || pressure2) ||
       !fluid_rvoice_mod_source_supported(definition->src1, definition->flags1) ||
       !fluid_rvoice_mod_source_supported(definition->src2, definition->flags2))
    {
        return FLUID_FAILED;
    }

    record->definition.dest = definition->dest;
    record->definition.src1 = definition->src1;
    record->definition.flags1 = definition->flags1;
    record->definition.src2 = definition->src2;
    record->definition.flags2 = definition->flags2;
    record->definition.trans = definition->trans;
    record->definition.amount = definition->amount;

    record->source[0].value = value1;
    record->source[0].range = range1;
    record->source[0].slew_enabled = pressure1;
    fluid_slew_init(&record->source[0].slew, pressure1 ? value1 / 127.0 : 0.0);

    record->source[1].value = value2;
    record->source[1].range = range2;
    record->source[1].slew_enabled = pressure2;
    fluid_slew_init(&record->source[1].slew, pressure2 ? value2 / 127.0 : 0.0);

    record->full_range_samples = full_range_samples;
    record->expression_full_range_samples = expression_samples;
    record->configured = 1;
    return FLUID_OK;
}

static inline int
fluid_rvoice_mod_init(fluid_rvoice_mod_t *record, const fluid_mod_t *definition,
                     fluid_real_t value1, fluid_real_t range1,
                     fluid_real_t value2, fluid_real_t range2, double samples)
{
    return fluid_rvoice_mod_init_times(record, definition, value1, range1,
                                       value2, range2, samples, 0.0);
}

/* Producer-side helpers: enqueue copied scalar data, never write to record.
 * The producer must use FluidSynth's existing serialized event producer and
 * flush the handler after publishing the complete group of related updates.
 * record's address must remain valid until all referencing events are consumed.
 * These functions do not allocate, flush, or transfer ownership of a pointer.
 */
#ifdef __cplusplus
extern "C" {
#endif
int fluid_rvoice_mod_enqueue_minimum(fluid_rvoice_eventhandler_t *handler,
    fluid_rvoice_mod_t *record, fluid_real_t samples);

/* Consumer-side operations. Advance exactly once per rendered interval;
 * evaluation is separate so recomputing a destination cannot advance time.
 * Evaluation returns only this route's contribution, with no destination
 * subtotal or clipping. A cleared/unconfigured record contributes zero.
 */
void fluid_rvoice_mod_advance(fluid_rvoice_mod_t *record,
                             unsigned int elapsed_samples);
fluid_real_t fluid_rvoice_mod_evaluate(const fluid_rvoice_mod_t *record);

int fluid_rvoice_mod_enqueue_config(fluid_rvoice_eventhandler_t *handler,
                                   fluid_rvoice_mod_t *record,
                                   const fluid_mod_t *definition,
                                   fluid_real_t value1, fluid_real_t value2,
                                   fluid_real_t full_range_samples);
int fluid_rvoice_mod_config_valid(const fluid_mod_t *definition,
                                 fluid_real_t value1, fluid_real_t value2,
                                 fluid_real_t full_range_samples);
int fluid_rvoice_mod_config_valid_times(const fluid_mod_t *definition,
    fluid_real_t value1, fluid_real_t value2, fluid_real_t pressure_samples,
    fluid_real_t expression_samples);
int fluid_rvoice_mod_enqueue_config_times(fluid_rvoice_eventhandler_t *handler,
    fluid_rvoice_mod_t *record, const fluid_mod_t *definition,
    fluid_real_t value1, fluid_real_t value2, fluid_real_t pressure_samples,
    fluid_real_t expression_samples);
int fluid_rvoice_mod_enqueue_times(fluid_rvoice_eventhandler_t *handler,
    fluid_rvoice_mod_t *record, fluid_real_t pressure_samples, fluid_real_t expression_samples);
int fluid_rvoice_mod_enqueue_inputs(fluid_rvoice_eventhandler_t *handler,
                                   fluid_rvoice_mod_t *record,
                                   fluid_real_t value1, fluid_real_t value2);
int fluid_rvoice_mod_enqueue_clear(fluid_rvoice_eventhandler_t *handler,
                                  fluid_rvoice_mod_t *record);
int fluid_rvoice_mod_enqueue_duration(fluid_rvoice_eventhandler_t *handler,
                                     fluid_rvoice_mod_t *record,
                                     fluid_real_t full_range_samples);

#ifdef __cplusplus
}
#endif

#endif /* FLUID_RVOICE_MOD_H */
