#include "test.h"
#include "fluid_rvoice_mod.h"
#include "fluid_rvoice_event.h"

#include <atomic>
#include <cmath>
#include <thread>

enum { Slots = 4, QueueSize = 8, Updates = 12000 };

struct stress_context
{
    fluid_rvoice_eventhandler_t handler{};
    fluid_rvoice_mod_t records[Slots]{};
    std::atomic<bool> saw_full{false};
    std::atomic<bool> done{false};
    std::atomic<int> rendered{0};
    int checked = 0; /* Consumer-owned until both threads have joined. */
};

/* Runs on the consumer, in FIFO order immediately after the update it checks.
 * The producer never reads records, not even to check their configured flag.
 */
static DECLARE_FLUID_RVOICE_FUNCTION(check_event)
{
    auto *context = static_cast<stress_context *>(obj);
    int sequence = param[0].i;
    int slot = param[1].i;
    bool configured = param[4].i != 0;
    const auto &record = context->records[slot];

    TEST_ASSERT(sequence == context->checked++);
    TEST_ASSERT((record.configured != 0) == configured);
    if(!configured)
    {
        TEST_ASSERT(fluid_rvoice_mod_evaluate(&record) == 0.0);
        return;
    }

    TEST_ASSERT(record.definition.amount == 128.0 + slot);
    TEST_ASSERT(record.definition.src1 == FLUID_MOD_KEYPRESSURE);
    TEST_ASSERT(record.definition.src2 == FLUID_MOD_CHANNELPRESSURE);
    TEST_ASSERT(record.definition.mapping_func == nullptr);
    TEST_ASSERT(record.definition.data == nullptr && record.definition.next == nullptr);
    for(int i = 0; i < 2; ++i)
    {
        double input = param[2 + i].real;
        TEST_ASSERT(record.source[i].value == input);
        TEST_ASSERT(std::fabs(record.source[i].slew.target - input / 127.0) < 1e-10);
        TEST_ASSERT(std::isfinite(record.source[i].slew.current));
        TEST_ASSERT(record.source[i].slew.current >= 0.0);
        TEST_ASSERT(record.source[i].slew.current <= 1.0);
    }
}

static void enqueue_check(stress_context &context, int sequence, int slot,
                          int input1, int input2, bool configured)
{
    fluid_rvoice_param_t param[MAX_EVENT_PARAMS] = {};
    param[0].i = sequence;
    param[1].i = slot;
    param[2].real = input1;
    param[3].real = input2;
    param[4].i = configured;
    TEST_SUCCESS(fluid_rvoice_eventhandler_push(&context.handler,
                                               check_event, &context, param));
}

/* One producer only, as required by the existing queue. Reserve room for the
 * complete batch before pushing: another producer must not share this helper.
 * The consumer can only free capacity, so this check remains conservative.
 */
static void wait_for_capacity(stress_context &context, int count)
{
    while(fluid_ringbuffer_get_inptr(context.handler.queue,
          fluid_atomic_int_get(&context.handler.queue_stored) + count - 1) == nullptr)
    {
        context.saw_full.store(true, std::memory_order_release);
        std::this_thread::yield();
    }
}

static void configure(stress_context &context, int slot, int value1, int value2)
{
    fluid_mod_t definition = {};
    definition.dest = GEN_ATTENUATION;
    definition.src1 = FLUID_MOD_KEYPRESSURE;
    definition.src2 = FLUID_MOD_CHANNELPRESSURE;
    definition.amount = 128.0 + slot;
    TEST_SUCCESS(fluid_rvoice_mod_enqueue_config(&context.handler,
        &context.records[slot], &definition, value1, value2, 1270.25));
    /* definition goes out of scope before the consumer may see the event. */
}

int main(void)
{
    stress_context context;
    context.handler.queue = new_fluid_ringbuffer(QueueSize, sizeof(fluid_rvoice_event_t));
    TEST_ASSERT(context.handler.queue != nullptr);
    int published = 0;

    std::thread consumer([&context]()
    {
        /* Force the queue to fill at least once, rather than hoping scheduling
         * happens to create backpressure. No record is inspected here.
         */
        while(!context.saw_full.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        while(!context.done.load(std::memory_order_acquire) ||
              fluid_rvoice_eventhandler_dispatch_count(&context.handler) != 0)
        {
            fluid_rvoice_eventhandler_dispatch_all(&context.handler);
            for(auto &record : context.records)
            {
                fluid_rvoice_mod_advance(&record, 64);
                auto value = fluid_rvoice_mod_evaluate(&record);
                TEST_ASSERT(std::isfinite(value) && value >= 0.0 && value <= 132.0);
            }
            context.rendered.fetch_add(1, std::memory_order_release);
            std::this_thread::yield();
        }
    });

    std::thread producer([&context, &published]()
    {
        int sequence = 0;
        /* Eight events exactly fill the queue. The consumer waits until the
         * next batch encounters that full queue before it starts draining.
         */
        for(int slot = 0; slot < Slots; ++slot)
        {
            configure(context, slot, 16 + slot, 32 + slot);
            enqueue_check(context, sequence++, slot, 16 + slot, 32 + slot, true);
        }
        fluid_rvoice_eventhandler_flush(&context.handler);

        for(int i = 0; i < Updates; ++i)
        {
            int slot = i % Slots;
            int value1 = i % 128;
            int value2 = (i * 7) % 128;
            bool reset = i % 53 == 0;
            wait_for_capacity(context, reset ? 5 : 2);
            if(reset)
            {
                TEST_SUCCESS(fluid_rvoice_mod_enqueue_clear(&context.handler,
                                                            &context.records[slot]));
                enqueue_check(context, sequence++, slot, 0, 0, false);
                configure(context, slot, value2, value1);
            }
            TEST_SUCCESS(fluid_rvoice_mod_enqueue_inputs(&context.handler,
                &context.records[slot], value1, value2));
            enqueue_check(context, sequence++, slot, value1, value2, true);

            /* Occasionally let the consumer run while these events remain
             * unpublished. It must not observe those queue slots yet.
             */
            if(i % 17 == 0)
            {
                std::this_thread::yield();
            }
            int previous_blocks = context.rendered.load(std::memory_order_acquire);
            fluid_rvoice_eventhandler_flush(&context.handler);
            if(i % 256 == 0)
            {
                /* Ensure rendering advances during the producer's lifetime,
                 * not merely once after all messages have been dispatched.
                 */
                while(context.rendered.load(std::memory_order_acquire) <= previous_blocks)
                {
                    std::this_thread::yield();
                }
            }
        }
        published = sequence;
        context.done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    TEST_ASSERT(context.checked == published);
    TEST_ASSERT(context.saw_full.load());
    TEST_ASSERT(context.rendered.load() > 0);
    TEST_ASSERT(fluid_rvoice_eventhandler_dispatch_count(&context.handler) == 0);
    TEST_ASSERT(fluid_atomic_int_get(&context.handler.queue_stored) == 0);
    delete_fluid_ringbuffer(context.handler.queue);
    return EXIT_SUCCESS;
}
