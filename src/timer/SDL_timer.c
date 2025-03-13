/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include "SDL_timer_c.h"
#include "../thread/SDL_systhread.h"

// #define DEBUG_TIMERS

#if !defined(SDL_PLATFORM_EMSCRIPTEN) || !defined(SDL_THREADS_DISABLED)

typedef struct SDL_Timer
{
    SDL_TimerID timerID;
    SDL_TimerCallback callback_ms;
    SDL_NSTimerCallback callback_ns;
    void *userdata;
    Uint64 interval;
    Uint64 scheduled;
    SDL_AtomicInt canceled;
    struct SDL_Timer *next;
} SDL_Timer;

typedef struct SDL_TimerMap
{
    SDL_TimerID timerID;
    SDL_Timer *timer;
    struct SDL_TimerMap *next;
} SDL_TimerMap;

// The timers are kept in a sorted list
typedef struct
{
    // Data used by the main thread
    SDL_InitState init;
    SDL_Thread *thread;
    SDL_TimerMap *timermap;
    SDL_Mutex *timermap_lock;

    // Padding to separate cache lines between threads
    char cache_pad[SDL_CACHELINE_SIZE];

    // Data used to communicate with the timer thread
    SDL_SpinLock lock;
    SDL_Semaphore *sem;
    SDL_Timer *pending;
    SDL_Timer *freelist;
    SDL_AtomicInt active;

    // List of timers - this is only touched by the timer thread
    SDL_Timer *timers;
} SDL_TimerData;

static SDL_TimerData SDL_timer_data;

/* The idea here is that any thread might add a timer, but a single
 * thread manages the active timer queue, sorted by scheduling time.
 *
 * Timers are removed by simply setting a canceled flag
 */

static void SDL_AddTimerInternal(SDL_TimerData *data, SDL_Timer *timer)
{
    SDL_Timer *prev, *curr;

    prev = NULL;
    for (curr = data->timers; curr; prev = curr, curr = curr->next) {
        if (curr->scheduled > timer->scheduled) {
            break;
        }
    }

    // Insert the timer here!
    if (prev) {
        prev->next = timer;
    } else {
        data->timers = timer;
    }
    timer->next = curr;
}

static int SDLCALL SDL_TimerThread(void *_data)
{
    SDL_TimerData *data = (SDL_TimerData *)_data;
    SDL_Timer *pending;
    SDL_Timer *current;
    SDL_Timer *freelist_head = NULL;
    SDL_Timer *freelist_tail = NULL;
    Uint64 tick, now, interval, delay;

    /* Threaded timer loop:
     *  1. Queue timers added by other threads
     *  2. Handle any timers that should dispatch this cycle
     *  3. Wait until next dispatch time or new timer arrives
     */
    for (;;) {
        // Pending and freelist maintenance
        SDL_LockSpinlock(&data->lock);
        {
            // Get any timers ready to be queued
            pending = data->pending;
            data->pending = NULL;

            // Make any unused timer structures available
            if (freelist_head) {
                freelist_tail->next = data->freelist;
                data->freelist = freelist_head;
            }
        }
        SDL_UnlockSpinlock(&data->lock);

        // Sort the pending timers into our list
        while (pending) {
            current = pending;
            pending = pending->next;
            SDL_AddTimerInternal(data, current);
        }
        freelist_head = NULL;
        freelist_tail = NULL;

        // Check to see if we're still running, after maintenance
        if (!SDL_GetAtomicInt(&data->active)) {
            break;
        }

        // Initial delay if there are no timers
        delay = (Uint64)-1;

        tick = SDL_GetTicksNS();

        // Process all the pending timers for this tick
        while (data->timers) {
            current = data->timers;

            if (tick < current->scheduled) {
                // Scheduled for the future, wait a bit
                delay = (current->scheduled - tick);
                break;
            }

            // We're going to do something with this timer
            data->timers = current->next;

            if (SDL_GetAtomicInt(&current->canceled)) {
                interval = 0;
            } else {
                if (current->callback_ms) {
                    interval = SDL_MS_TO_NS(current->callback_ms(current->userdata, current->timerID, (Uint32)SDL_NS_TO_MS(current->interval)));
                } else {
                    interval = current->callback_ns(current->userdata, current->timerID, current->interval);
                }
            }

            if (interval > 0) {
                // Reschedule this timer
                current->interval = interval;
                current->scheduled = tick + interval;
                SDL_AddTimerInternal(data, current);
            } else {
                if (!freelist_head) {
                    freelist_head = current;
                }
                if (freelist_tail) {
                    freelist_tail->next = current;
                }
                freelist_tail = current;

                SDL_SetAtomicInt(&current->canceled, 1);
            }
        }

        // Adjust the delay based on processing time
        now = SDL_GetTicksNS();
        interval = (now - tick);
        if (interval > delay) {
            delay = 0;
        } else {
            delay -= interval;
        }

        /* Note that each time a timer is added, this will return
           immediately, but we process the timers added all at once.
           That's okay, it just means we run through the loop a few
           extra times.
         */
        SDL_WaitSemaphoreTimeoutNS(data->sem, delay);
    }
    return 0;
}

bool SDL_InitTimers(void)
{
    SDL_TimerData *data = &SDL_timer_data;

    if (!SDL_ShouldInit(&data->init)) {
        return true;
    }

    data->timermap_lock = SDL_CreateMutex();
    if (!data->timermap_lock) {
        goto error;
    }

    data->sem = SDL_CreateSemaphore(0);
    if (!data->sem) {
        goto error;
    }

    SDL_SetAtomicInt(&data->active, true);

    // Timer threads use a callback into the app, so we can't set a limited stack size here.
    data->thread = SDL_CreateThread(SDL_TimerThread, "SDLTimer", data);
    if (!data->thread) {
        goto error;
    }

    SDL_SetInitialized(&data->init, true);
    return true;

error:
    SDL_SetInitialized(&data->init, true);
    SDL_QuitTimers();
    return false;
}

void SDL_QuitTimers(void)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_Timer *timer;
    SDL_TimerMap *entry;

    if (!SDL_ShouldQuit(&data->init)) {
        return;
    }

    SDL_SetAtomicInt(&data->active, false);

    // Shutdown the timer thread
    if (data->thread) {
        SDL_SignalSemaphore(data->sem);
        SDL_WaitThread(data->thread, NULL);
        data->thread = NULL;
    }

    if (data->sem) {
        SDL_DestroySemaphore(data->sem);
        data->sem = NULL;
    }

    // Clean up the timer entries
    while (data->timers) {
        timer = data->timers;
        data->timers = timer->next;
        SDL_free(timer);
    }
    while (data->freelist) {
        timer = data->freelist;
        data->freelist = timer->next;
        SDL_free(timer);
    }
    while (data->timermap) {
        entry = data->timermap;
        data->timermap = entry->next;
        SDL_free(entry);
    }

    if (data->timermap_lock) {
        SDL_DestroyMutex(data->timermap_lock);
        data->timermap_lock = NULL;
    }

    SDL_SetInitialized(&data->init, false);
}

static bool SDL_CheckInitTimers(void)
{
    return SDL_InitTimers();
}

static SDL_TimerID SDL_CreateTimer(Uint64 interval, SDL_TimerCallback callback_ms, SDL_NSTimerCallback callback_ns, void *userdata)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_Timer *timer;
    SDL_TimerMap *entry;

    if (!callback_ms && !callback_ns) {
        SDL_InvalidParamError("callback");
        return 0;
    }

    if (!SDL_CheckInitTimers()) {
        return 0;
    }

    SDL_LockSpinlock(&data->lock);
    timer = data->freelist;
    if (timer) {
        data->freelist = timer->next;
    }
    SDL_UnlockSpinlock(&data->lock);

    if (timer) {
        SDL_RemoveTimer(timer->timerID);
    } else {
        timer = (SDL_Timer *)SDL_malloc(sizeof(*timer));
        if (!timer) {
            return 0;
        }
    }
    timer->timerID = SDL_GetNextObjectID();
    timer->callback_ms = callback_ms;
    timer->callback_ns = callback_ns;
    timer->userdata = userdata;
    timer->interval = interval;
    timer->scheduled = SDL_GetTicksNS() + timer->interval;
    SDL_SetAtomicInt(&timer->canceled, 0);

    entry = (SDL_TimerMap *)SDL_malloc(sizeof(*entry));
    if (!entry) {
        SDL_free(timer);
        return 0;
    }
    entry->timer = timer;
    entry->timerID = timer->timerID;

    SDL_LockMutex(data->timermap_lock);
    entry->next = data->timermap;
    data->timermap = entry;
    SDL_UnlockMutex(data->timermap_lock);

    // Add the timer to the pending list for the timer thread
    SDL_LockSpinlock(&data->lock);
    timer->next = data->pending;
    data->pending = timer;
    SDL_UnlockSpinlock(&data->lock);

    // Wake up the timer thread if necessary
    SDL_SignalSemaphore(data->sem);

    return entry->timerID;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata)
{
    return SDL_CreateTimer(SDL_MS_TO_NS(interval), callback, NULL, userdata);
}

SDL_TimerID SDL_AddTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata)
{
    return SDL_CreateTimer(interval, NULL, callback, userdata);
}

bool SDL_RemoveTimer(SDL_TimerID id)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_TimerMap *prev, *entry;
    bool canceled = false;

    if (!id) {
        return SDL_InvalidParamError("id");
    }

    // Find the timer
    SDL_LockMutex(data->timermap_lock);
    prev = NULL;
    for (entry = data->timermap; entry; prev = entry, entry = entry->next) {
        if (entry->timerID == id) {
            if (prev) {
                prev->next = entry->next;
            } else {
                data->timermap = entry->next;
            }
            break;
        }
    }
    SDL_UnlockMutex(data->timermap_lock);

    if (entry) {
        if (!SDL_GetAtomicInt(&entry->timer->canceled)) {
            SDL_SetAtomicInt(&entry->timer->canceled, 1);
            canceled = true;
        }
        SDL_free(entry);
    }
    if (canceled) {
        return true;
    } else {
        return SDL_SetError("Timer not found");
    }
}

#else

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

typedef struct SDL_TimerMap
{
    SDL_TimerID timerID;
    int timeoutID;
    Uint64 interval;
    SDL_TimerCallback callback_ms;
    SDL_NSTimerCallback callback_ns;
    void *userdata;
    struct SDL_TimerMap *next;
} SDL_TimerMap;

typedef struct
{
    SDL_TimerMap *timermap;
} SDL_TimerData;

static SDL_TimerData SDL_timer_data;

static void SDL_Emscripten_TimerHelper(void *userdata)
{
    SDL_TimerMap *entry = (SDL_TimerMap *)userdata;
    if (entry->callback_ms) {
        entry->interval = SDL_MS_TO_NS(entry->callback_ms(entry->userdata, entry->timerID, (Uint32)SDL_NS_TO_MS(entry->interval)));
    } else {
        entry->interval = entry->callback_ns(entry->userdata, entry->timerID, entry->interval);
    }
    if (entry->interval > 0) {
        entry->timeoutID = emscripten_set_timeout(&SDL_Emscripten_TimerHelper,
                                                  SDL_NS_TO_MS(entry->interval),
                                                  entry);
    }
}

bool SDL_InitTimers(void)
{
    return true;
}

void SDL_QuitTimers(void)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_TimerMap *entry;

    while (data->timermap) {
        entry = data->timermap;
        data->timermap = entry->next;
        SDL_free(entry);
    }
}

static SDL_TimerID SDL_CreateTimer(Uint64 interval, SDL_TimerCallback callback_ms, SDL_NSTimerCallback callback_ns, void *userdata)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_TimerMap *entry;

    if (!callback_ms && !callback_ns) {
        SDL_InvalidParamError("callback");
        return 0;
    }

    entry = (SDL_TimerMap *)SDL_malloc(sizeof(*entry));
    if (!entry) {
        return 0;
    }
    entry->timerID = SDL_GetNextObjectID();
    entry->callback_ms = callback_ms;
    entry->callback_ns = callback_ns;
    entry->userdata = userdata;
    entry->interval = interval;

    entry->timeoutID = emscripten_set_timeout(&SDL_Emscripten_TimerHelper,
                                              SDL_NS_TO_MS(entry->interval),
                                              entry);

    entry->next = data->timermap;
    data->timermap = entry;

    return entry->timerID;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback callback, void *userdata)
{
    return SDL_CreateTimer(SDL_MS_TO_NS(interval), callback, NULL, userdata);
}

SDL_TimerID SDL_AddTimerNS(Uint64 interval, SDL_NSTimerCallback callback, void *userdata)
{
    return SDL_CreateTimer(interval, NULL, callback, userdata);
}

bool SDL_RemoveTimer(SDL_TimerID id)
{
    SDL_TimerData *data = &SDL_timer_data;
    SDL_TimerMap *prev, *entry;

    if (!id) {
        return SDL_InvalidParamError("id");
    }

    // Find the timer
    prev = NULL;
    for (entry = data->timermap; entry; prev = entry, entry = entry->next) {
        if (entry->timerID == id) {
            if (prev) {
                prev->next = entry->next;
            } else {
                data->timermap = entry->next;
            }
            break;
        }
    }

    if (entry) {
        emscripten_clear_timeout(entry->timeoutID);
        SDL_free(entry);
        return true;
    } else {
        return SDL_SetError("Timer not found");
    }
}

#endif // !SDL_PLATFORM_EMSCRIPTEN || !SDL_THREADS_DISABLED

static Uint64 tick_start;
static Uint32 tick_numerator_ns;
static Uint32 tick_denominator_ns;
static Uint32 tick_numerator_ms;
static Uint32 tick_denominator_ms;

#if defined(SDL_TIMER_WINDOWS) && !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
#include <mmsystem.h>
#define HAVE_TIME_BEGIN_PERIOD
#endif

static void SDL_SetSystemTimerResolutionMS(int period)
{
#ifdef HAVE_TIME_BEGIN_PERIOD
    static int timer_period = 0;

    if (period != timer_period) {
        if (timer_period) {
            timeEndPeriod((UINT)timer_period);
        }

        timer_period = period;

        if (timer_period) {
            timeBeginPeriod((UINT)timer_period);
        }
    }
#endif // HAVE_TIME_BEGIN_PERIOD
}

static void SDLCALL SDL_TimerResolutionChanged(void *userdata, const char *name, const char *oldValue, const char *hint)
{
    int period;

    // Unless the hint says otherwise, let's have good sleep precision
    if (hint && *hint) {
        period = SDL_atoi(hint);
    } else {
        period = 1;
    }
    if (period || oldValue != hint) {
        SDL_SetSystemTimerResolutionMS(period);
    }
}

void SDL_InitTicks(void)
{
    Uint64 tick_freq;
    Uint32 gcd;

    if (tick_start) {
        return;
    }

    /* If we didn't set a precision, set it high. This affects lots of things
       on Windows besides the SDL timers, like audio callbacks, etc. */
    SDL_AddHintCallback(SDL_HINT_TIMER_RESOLUTION,
                        SDL_TimerResolutionChanged, NULL);

    tick_freq = SDL_GetPerformanceFrequency();
    SDL_assert(tick_freq > 0 && tick_freq <= (Uint64)SDL_MAX_UINT32);

    gcd = SDL_CalculateGCD(SDL_NS_PER_SECOND, (Uint32)tick_freq);
    tick_numerator_ns = (SDL_NS_PER_SECOND / gcd);
    tick_denominator_ns = (Uint32)(tick_freq / gcd);

    gcd = SDL_CalculateGCD(SDL_MS_PER_SECOND, (Uint32)tick_freq);
    tick_numerator_ms = (SDL_MS_PER_SECOND / gcd);
    tick_denominator_ms = (Uint32)(tick_freq / gcd);

    tick_start = SDL_GetPerformanceCounter();
    if (!tick_start) {
        --tick_start;
    }
}

void SDL_QuitTicks(void)
{
    SDL_RemoveHintCallback(SDL_HINT_TIMER_RESOLUTION,
                        SDL_TimerResolutionChanged, NULL);

    SDL_SetSystemTimerResolutionMS(0); // always release our timer resolution request.

    tick_start = 0;
}

Uint64 SDL_GetTicksNS(void)
{
    Uint64 starting_value, value;

    if (!tick_start) {
        SDL_InitTicks();
    }

    starting_value = (SDL_GetPerformanceCounter() - tick_start);
    value = (starting_value * tick_numerator_ns);
    SDL_assert(value >= starting_value);
    value /= tick_denominator_ns;
    return value;
}

Uint64 SDL_GetTicks(void)
{
    Uint64 starting_value, value;

    if (!tick_start) {
        SDL_InitTicks();
    }

    starting_value = (SDL_GetPerformanceCounter() - tick_start);
    value = (starting_value * tick_numerator_ms);
    SDL_assert(value >= starting_value);
    value /= tick_denominator_ms;
    return value;
}

void SDL_Delay(Uint32 ms)
{
    SDL_SYS_DelayNS(SDL_MS_TO_NS(ms));
}

void SDL_DelayNS(Uint64 ns)
{
    SDL_SYS_DelayNS(ns);
}

void SDL_DelayPrecise(Uint64 ns)
{
    // Non-hard-realtime platforms simply cannot give you the actual sleep
    // amount you want in a single delay request (SDL_Delay(), SDL_DelayNS()).
    // A more sophisticated approach to sleeping is required, to get the amount
    // you actually want.
    //
    // An unavoidable consequence of doing more computation than a trivial,
    // single delay request is that precise delay always consumes more
    // CPU/power than a single delay request, but this algorithm is
    // aggressively designed to keep that under control. In fact, it actually
    // gets increasingly CPU/power efficient over the total delay duration, as
    // the total delay duration (the ns parameter) increases.
    //
    // This algorithm is designed around some assumptions that seem to always
    // hold across all platforms: When you request a delay duration N, on
    // average you get an actual sleep N + M, where M tends to be close to a
    // fixed value on average over a short term of repeated constant N delay
    // requests, with M being relatively small, generally below 1 ms. M can
    // occasionally spike to far higher values, perhaps several ms, due to
    // platform quirks (I've observed such behavior on Windows 10/x86_64, which
    // tends to sporadically produce overshoot spikes on the thread handling
    // video @nightmareci) or during high system load (CPU usage is near/at
    // 100%). Additionally, requesting delays of zero duration produce a small,
    // roughly-constant sleep, more than CPU pausing does, while consuming less
    // CPU/power than CPU pauses.
    //
    // Some platforms, namely Emscripten, only support 1 ms resolution sleeps,
    // so as much of the sleep *must* be done with delay requests >=1 ms, doing
    // as few sub-1 ms delays as possible at the end.
    //
    // Summary of the delay steps in the precise delay algorithm:
    // 1. Bigger sleeps, when ns is large; reduces CPU/power usage a lot on
    //    some platforms (Activity Monitor on my M1 Mac mini running macOS
    //    seems to report this behavior @nightmareci), the algorithm's
    //    CPU/power efficiency here exponentially proportional to the total
    //    delay.
    // 2. 1 ms sleeps when only a few ms is left to delay, while trying to
    //    undershoot.
    // 3. 1 ms sleeps, accepting we may overshoot, when >2 ms is left by step 2
    //    (step 2 undershot by >2 ms). Step 2 is designed to likely not
    //    undershoot by much, so this step is often skipped.
    // 4. Zero-duration sleeps, as they may consume less CPU/power than CPU
    //    pauses.
    // 5. Spin loop using CPU pauses for the small remaining time.

    Uint64 current_value = SDL_GetTicksNS();
    const Uint64 target_value = current_value + ns;

    // The short sleep duration to use when close to the deadline.
    // We'll use 1 ms, it's the minimum guaranteed to produce real sleeps across
    // all platforms.
    const Uint64 SHORT_SLEEP_NS = 1 * SDL_NS_PER_MS;

    if (ns <= 2 * SHORT_SLEEP_NS) {
        goto step_4;
    }

    // Delay step 1
    // This loop does longish (>=10 ms) iterative sleeps to reduce power usage
    // for larger values of ns, aiming to undershoot by a smallish amount so
    // the 1 ms loop below doesn't need to run that many times. The target
    // sleep duration, target_sleep_ns, is reduced as the deadline approaches,
    // similar to Zeno's paradox of motion. The host platform may deprioritize
    // the current thread for large values of ns (>=1 s or so), resulting in
    // larger overshoots in later steps vs. smaller total sleeps, but that
    // can't be helped if we want to consume less CPU/power than never doing
    // longish sleeps. And, in practice, the later steps still give more
    // precise delay than an ordinary SDL_DelayNS(), producing a pretty
    // consistent and small small overshoot vs. large and highly inconsistent
    // with just SDL_DelayNS().
    Uint64 target_sleep_ns = ns / 10;
    Uint64 max_overshoot_ns = 0;
    if (target_sleep_ns >= (10 * SHORT_SLEEP_NS)) {
        Uint64 current_sleep_ns = target_sleep_ns - SHORT_SLEEP_NS;
        while ((current_sleep_ns >= 10 * SHORT_SLEEP_NS) && ((current_value + target_sleep_ns + (10 * SHORT_SLEEP_NS)) < target_value)) {
            SDL_SYS_DelayNS(current_sleep_ns);
            const Uint64 now = SDL_GetTicksNS();
            if (now >= target_value) {
                return;
            }
            const Uint64 overshoot_ns = (now - current_value) - current_sleep_ns;
            if (overshoot_ns > max_overshoot_ns) {
                max_overshoot_ns = overshoot_ns;
            }
            if (max_overshoot_ns >= target_sleep_ns) {
                max_overshoot_ns = 0;
            }
            current_value = now;
            if ((current_value + target_sleep_ns + (10 * SHORT_SLEEP_NS)) > target_value) {
                for (
                    target_sleep_ns = (target_value - current_value) / 10;
                    (target_sleep_ns > SHORT_SLEEP_NS) && ((current_value + target_sleep_ns + (10 * SHORT_SLEEP_NS)) > target_value);
                    target_sleep_ns /= 10
                );
                if (target_sleep_ns <= SHORT_SLEEP_NS) {
                    break;
                }
                if (max_overshoot_ns >= target_sleep_ns) {
                    max_overshoot_ns = 0;
                }
            }
            current_sleep_ns = target_sleep_ns - max_overshoot_ns;
        }
    }

    // Delay step 2
    // When there's only a few ms left to sleep, we want to be sure to do as
    // much of the remaining sleep as possible with 1 ms sleeps, to keep
    // CPU/power usage down on 1 ms precision platforms.
    //
    // We keep track of the maximum sleep duration as we do iterative sleeps,
    // to better guarantee we don't overshoot on the last iteration. Due to the
    // high variability in delay calls on basically all platforms at all times,
    // it's not correct to cache the maximum long-term. In practice, only
    // keeping track of it per loop while the sleep duration isn't changing
    // works out well.
    Uint64 max_sleep_ns = SHORT_SLEEP_NS;
    if (max_overshoot_ns < max_sleep_ns) {
        max_sleep_ns += max_overshoot_ns;
    }
    while ((current_value + max_sleep_ns) < target_value) {
        SDL_SYS_DelayNS(SHORT_SLEEP_NS);
        const Uint64 now = SDL_GetTicksNS();
        if (now >= target_value) {
            return;
        }
        const Uint64 next_sleep_ns = (now - current_value);
        if (next_sleep_ns > max_sleep_ns) {
            max_sleep_ns = next_sleep_ns;
        }
        current_value = now;
    }

    // Delay step 3
    // The above undershoot-attempting loop may undershoot several ms, but is
    // unlikely to do so in practice, as individual delay request overshoots
    // tend to be under 1 ms. This ensures we do 1 ms sleeps to avoid high
    // CPU/power usage in such cases.
    while ((current_value + (2 * SHORT_SLEEP_NS)) < target_value) {
        SDL_SYS_DelayNS(SHORT_SLEEP_NS);
        current_value = SDL_GetTicksNS();
        if (current_value >= target_value) {
            return;
        }
    }

    // Delay step 4
    // Zero-duration sleeps on many platforms consume less CPU/power than CPU
    // pauses, so let's take advantage of that here. They're usually less than
    // 1 ms, but are sometimes over 0.5 ms, so if there's <= 1 ms remaining, we
    // can't reliably undershoot anymore. Nonzero sleeps below 1 ms have pretty
    // inconsistent behavior on some platforms, even platforms supporting
    // better than 1 ms resolution sleeps, so we don't use them here.
step_4:
    while ((current_value + SHORT_SLEEP_NS) < target_value) {
        SDL_SYS_DelayNS(0);
        current_value = SDL_GetTicksNS();
    }

    // Delay step 5
    // Spin for any remaining time
    while (current_value < target_value) {
        SDL_CPUPauseInstruction();
        current_value = SDL_GetTicksNS();
    }
}
