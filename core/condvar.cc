/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/condvar.h>
#include <sched.h>
#include <errno.h>
#include <osv/trace.hh>
#include <osv/wait_record.hh>

TRACEPOINT(trace_condvar_wait, "%p", condvar *);
TRACEPOINT(trace_condvar_wake_one, "%p", condvar *);
TRACEPOINT(trace_condvar_wake_all, "%p", condvar *);

int condvar_wait(condvar_t *condvar, mutex_t* user_mutex, uint64_t expiration)
{
    if (expiration) {
        sched::timer timer(*sched::thread::current());
        timer.set(expiration);
        return condvar_wait(condvar, user_mutex, &timer);
    } else {
        return condvar_wait(condvar, user_mutex, nullptr);
    }
}

int condvar_wait(condvar_t *condvar, mutex_t* user_mutex, sched::timer* tmr)
{
    trace_condvar_wait(condvar);
    int ret = 0;
    wait_record wr(sched::thread::current());

    mutex_lock(&condvar->m);
    if (!condvar->waiters_fifo.oldest) {
        condvar->waiters_fifo.oldest = &wr;
    } else {
        condvar->waiters_fifo.newest->next = &wr;
    }
    condvar->waiters_fifo.newest = &wr;
#if WAIT_MORPHING
    // Remember user_mutex for "wait morphing" feature. Assert our assumption
    // that concurrent waits use the same mutex.
    assert(!condvar->user_mutex || condvar->user_mutex == user_mutex);
    condvar->user_mutex = user_mutex;
#endif
    // This preempt_disable() is just an optimization, to avoid context
    // switch between the two unlocks.
    sched::preempt_disable();
    mutex_unlock(user_mutex);
    mutex_unlock(&condvar->m);
    sched::preempt_enable();

    // Wait until either the timer expires or condition variable signaled
    wr.wait(tmr);
    if (!wr.woken()) {
        ret = ETIMEDOUT;
        // wr is still in the linked list (because of a timeout) so remove it:
        mutex_lock(&condvar->m);
        if (&wr == condvar->waiters_fifo.oldest) {
            condvar->waiters_fifo.oldest = wr.next;
            if (!wr.next) {
                condvar->waiters_fifo.newest = nullptr;
            }
        } else {
            wait_record *p = condvar->waiters_fifo.oldest;
            while (p) {
                if (&wr == p->next) {
                    p->next = p->next->next;
                    if(!p->next) {
                        condvar->waiters_fifo.newest = p;
                    }
                    break;
                }
                p = p->next;
            }
            if (!p) {
                ret = 0;
            }
        }
        mutex_unlock(&condvar->m);
        if (!ret) {
            // wr is no longer in the queue, so either wr.wake() is
            // already done, or wake_all() has just taken the whole queue
            // and will wr.wake() soon. We can't return (and invalidate wr)
            // until it calls wr.wake().
            wr.wait();
        }
    }

#if WAIT_MORPHING
    if (wr.woken()) {
        // Our wr was woken. The "wait morphing" protocol used by
        // condvar_wake*() ensures that this only happens after we got the
        // user_mutex for ourselves, so no need to mutex_lock() here.
        user_mutex->receive_lock();
    } else
#endif
        mutex_lock(user_mutex);

    return ret;
}

void condvar_wake_one(condvar_t *condvar)
{
    trace_condvar_wake_one(condvar);
    // To make wake with no waiters faster, and avoid unnecessary contention
    // in that case, first check the queue head outside the lock. If it is not
    // empty, we still need to take the lock, and re-read the head.
    if (!condvar->waiters_fifo.oldest) {
        return;
    }

    mutex_lock(&condvar->m);
    wait_record *wr = condvar->waiters_fifo.oldest;
    if (wr) {
        condvar->waiters_fifo.oldest = wr->next;
        if (wr->next == nullptr) {
            condvar->waiters_fifo.newest = nullptr;
        }
#if WAIT_MORPHING
        // Rather than wake the waiter here (wr->wake()) and have it wait
        // again for the mutex, we do "wait morphing" - have it continue to
        // sleep until the mutex becomes available.
        condvar->user_mutex->send_lock(wr);
        // To help the assert() in condvar_wait(), we need to zero saved
        // user_mutex when all concurrent condvar_wait()s are done.
        if (!condvar->waiters_fifo.oldest) {
            condvar->user_mutex = nullptr;
        }
#else
        wr->wake();
#endif
    }
    mutex_unlock(&condvar->m);
}

void condvar_wake_all(condvar_t *condvar)
{
    trace_condvar_wake_all(condvar);
    if (!condvar->waiters_fifo.oldest) {
        return;
    }

    mutex_lock(&condvar->m);
    wait_record *wr = condvar->waiters_fifo.oldest;
#if WAIT_MORPHING
    // To help the assert() in condvar_wait(), we need to zero saved
    // user_mutex when all concurrent condvar_wait()s are done.
    auto user_mutex = condvar->user_mutex;
    condvar->user_mutex = nullptr;
#endif
    condvar->waiters_fifo.oldest = condvar->waiters_fifo.newest = nullptr;
    mutex_unlock(&condvar->m);
    while (wr) {
        auto next_wr = wr->next; // need to save - *wr invalid after wake
#if WAIT_MORPHING
        auto cpu_wr = wr->thread()->tcpu();
        user_mutex->send_lock(wr);
        // As an optimization for many threads to wake up on relatively few
        // CPUs, queue all the threads that will likely wake on the same CPU
        // one after another, as same-CPU wakeup is faster.
        wait_record *prevr = nullptr;
        for (auto r = next_wr; r;) {
            auto nextr = r->next;
            if (r->thread()->tcpu() == cpu_wr) {
                user_mutex->send_lock(r);
                if (r == next_wr) {
                    next_wr = nextr;
                } else {
                    prevr->next = nextr;
                }
            } else {
                prevr = r;
            }
            r = nextr;
        }
#else
        wr->wake();
#endif
        wr = next_wr;
    }
}
