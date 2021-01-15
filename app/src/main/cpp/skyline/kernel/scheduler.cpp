// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <unistd.h>
#include <common/signal.h>
#include "types/KThread.h"
#include "scheduler.h"

namespace skyline::kernel {
    Scheduler::CoreContext::CoreContext(u8 id, u8 preemptionPriority) : id(id), preemptionPriority(preemptionPriority) {}

    Scheduler::Scheduler(const DeviceState &state) : state(state) {}

    void Scheduler::SignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls) {
        if (*tls) {
            const auto &state{*reinterpret_cast<nce::ThreadContext *>(*tls)->state};
            state.scheduler->Rotate(false);
            YieldPending = false;
            state.scheduler->WaitSchedule();
        } else {
            YieldPending = true;
        }
    }

    Scheduler::CoreContext &Scheduler::LoadBalance(const std::shared_ptr<type::KThread> &thread, bool alwaysInsert) {
        std::lock_guard migrationLock(thread->coreMigrationMutex);
        auto *currentCore{&cores.at(thread->coreId)};

        if (!currentCore->queue.empty() && thread->affinityMask.count() != 1) {
            // Select core where the current thread will be scheduled the earliest based off average timeslice durations for resident threads
            // There's a preference for the current core as migration isn't free
            size_t minTimeslice{};
            CoreContext *optimalCore{};
            for (auto &candidateCore : cores) {
                if (thread->affinityMask.test(candidateCore.id)) {
                    u64 timeslice{};

                    if (!candidateCore.queue.empty()) {
                        std::unique_lock coreLock(candidateCore.mutex);

                        auto threadIterator{candidateCore.queue.cbegin()};
                        if (threadIterator != candidateCore.queue.cend()) {
                            const auto &runningThread{*threadIterator};
                            timeslice += runningThread->averageTimeslice ? std::min(runningThread->averageTimeslice - (util::GetTimeTicks() - runningThread->timesliceStart), 1UL) : runningThread->timesliceStart ? util::GetTimeTicks() - runningThread->timesliceStart : 1UL;

                            while (++threadIterator != candidateCore.queue.cend()) {
                                const auto &residentThread{*threadIterator};
                                if (residentThread->priority <= thread->priority)
                                    timeslice += residentThread->averageTimeslice ? residentThread->averageTimeslice : 1UL;
                            }
                        }
                    }

                    if (!optimalCore || timeslice < minTimeslice || (timeslice == minTimeslice && &candidateCore == currentCore)) {
                        optimalCore = &candidateCore;
                        minTimeslice = timeslice;
                    }
                }
            }

            if (optimalCore != currentCore) {
                if (!alwaysInsert && thread == state.thread)
                    RemoveThread();
                else if (!alwaysInsert && thread != state.thread)
                    throw exception("Migrating an external thread (T{}) without 'alwaysInsert' isn't supported", thread->id);
                thread->coreId = optimalCore->id;
                InsertThread(thread);
                state.logger->Debug("Load Balancing T{}: C{} -> C{}", thread->id, currentCore->id, optimalCore->id);
            } else {
                if (alwaysInsert)
                    InsertThread(thread);
                state.logger->Debug("Load Balancing T{}: C{} (Late)", thread->id, currentCore->id);
            }

            return *optimalCore;
        }

        if (alwaysInsert)
            InsertThread(thread);
        state.logger->Debug("Load Balancing T{}: C{} (Early)", thread->id, currentCore->id);

        return *currentCore;
    }

    void Scheduler::InsertThread(const std::shared_ptr<type::KThread> &thread) {
        auto &core{cores.at(thread->coreId)};
        std::unique_lock lock(core.mutex);
        auto nextThread{std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority)};
        if (nextThread == core.queue.begin()) {
            if (nextThread != core.queue.end()) {
                // If the inserted thread has a higher priority than the currently running thread (and the queue isn't empty)
                if (state.thread == thread) {
                    // If the current thread is inserting itself then we try to optimize by trying to by forcefully yielding it ourselves now
                    // We can avoid waiting on it to yield itself on receiving the signal which serializes the entire pipeline
                    // This isn't done in other cases as this optimization is unsafe when done where serialization is required (Eg: Mutexes)
                    core.queue.front()->forceYield = true;
                    core.queue.splice(std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority), core.queue, core.queue.begin());
                    core.queue.push_front(thread);
                } else {
                    // If we're inserting another thread then we just insert it after the thread in line
                    // It'll automatically be ready to be scheduled when the thread at the front yields
                    // This enforces strict synchronization for the thread to run and waits till the previous thread has yielded itself
                    core.queue.insert(std::next(core.queue.begin()), thread);
                }

                if (state.thread != core.queue.front())
                    core.queue.front()->SendSignal(YieldSignal);
                else
                    YieldPending = true;
            } else {
                core.queue.push_front(thread);
            }
            if (thread != state.thread)
                thread->wakeCondition.notify_one(); // We only want to trigger the conditional variable if the current thread isn't inserting itself
        } else {
            core.queue.insert(nextThread, thread);
        }
    }

    void Scheduler::WaitSchedule(bool loadBalance) {
        auto &thread{state.thread};
        auto *core{&cores.at(thread->coreId)};

        std::unique_lock lock(core->mutex);
        if (loadBalance && thread->affinityMask.count() > 1) {
            std::chrono::milliseconds loadBalanceThreshold{PreemptiveTimeslice * 2}; //!< The amount of time that needs to pass unscheduled for a thread to attempt load balancing
            while (!thread->wakeCondition.wait_for(lock, loadBalanceThreshold, [&]() { return !core->queue.empty() && core->queue.front() == thread; })) {
                lock.unlock();
                LoadBalance(state.thread);
                if (thread->coreId == core->id) {
                    lock.lock();
                } else {
                    core = &cores.at(thread->coreId);
                    lock = std::unique_lock(core->mutex);
                }

                loadBalanceThreshold *= 2; // We double the duration required for future load balancing for this invocation to minimize pointless load balancing
            }
        } else {
            thread->wakeCondition.wait(lock, [&]() { return !core->queue.empty() && core->queue.front() == thread; });
        }

        if (thread->priority == core->preemptionPriority) {
            struct itimerspec spec{.it_value = {.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(PreemptiveTimeslice).count()}};
            timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
            thread->isPreempted = true;
        }

        thread->timesliceStart = util::GetTimeTicks();
    }

    bool Scheduler::TimedWaitSchedule(std::chrono::nanoseconds timeout) {
        auto &thread{state.thread};
        auto *core{&cores.at(thread->coreId)};

        std::unique_lock lock(core->mutex);
        if (thread->wakeCondition.wait_for(lock, timeout, [&]() { return !core->queue.empty() && core->queue.front() == thread; })) {
            if (thread->priority == core->preemptionPriority) {
                struct itimerspec spec{.it_value = {.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(PreemptiveTimeslice).count()}};
                timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
                thread->isPreempted = true;
            }

            thread->timesliceStart = util::GetTimeTicks();

            return true;
        } else {
            return false;
        }
    }

    void Scheduler::Rotate(bool cooperative) {
        auto &thread{state.thread};
        auto &core{cores.at(thread->coreId)};

        std::unique_lock lock(core.mutex);
        if (core.queue.front() == thread) {
            thread->averageTimeslice = (thread->averageTimeslice / 4) + (3 * (util::GetTimeTicks() - thread->timesliceStart / 4)); // 0.25 * old timeslice duration + 0.75 * current timeslice duration

            // Splice the linked element from the beginning of the queue to where it's priority is present
            core.queue.splice(std::upper_bound(core.queue.begin(), core.queue.end(), thread->priority.load(), type::KThread::IsHigherPriority), core.queue, core.queue.begin());

            auto front{core.queue.front()};
            if (front != thread)
                front->wakeCondition.notify_one(); // If we aren't at the front of the queue, only then should we wake the thread at the front up

            if (cooperative && thread->isPreempted) {
                // If a preemptive thread did a cooperative yield then we need to disarm the preemptive timer
                struct itimerspec spec{};
                timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
            }

            thread->isPreempted = false;
        } else if (thread->forceYield) {
            // We need to check if this thread was yielded by another thread on behalf of this thread
            // If it is then we just need to disarm the preemption timer and update the average timeslice
            // If it isn't then we need to throw an exception as it's indicative of an invalid state
            struct ThreadComparision {
                constexpr bool operator()(const i8 priority, const std::shared_ptr<type::KThread> &it) { return priority < it->priority; }

                constexpr bool operator()(const std::shared_ptr<type::KThread> &it, const i8 priority) { return it->priority < priority; }
            };
            auto bounds{std::equal_range(core.queue.begin(), core.queue.end(), thread->priority.load(), ThreadComparision{})};

            auto iterator{std::find(bounds.first, bounds.second, thread)};
            if (iterator != bounds.second) {
                thread->averageTimeslice = (thread->averageTimeslice / 4) + (3 * (util::GetTimeTicks() - thread->timesliceStart / 4));

                if (cooperative && thread->isPreempted) {
                    struct itimerspec spec{};
                    timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
                }

                thread->isPreempted = false;
            } else {
                throw exception("T{} called Rotate while not being in C{}'s queue after being forcefully yielded", thread->id, thread->coreId);
            }
        } else {
            throw exception("T{} called Rotate while not being in C{}'s queue", thread->id, thread->coreId);
        }
        thread->forceYield = false;
    }

    void Scheduler::UpdatePriority(const std::shared_ptr<type::KThread> &thread) {
        std::lock_guard migrationLock(thread->coreMigrationMutex);
        auto *core{&cores.at(thread->coreId)};
        std::unique_lock coreLock(core->mutex);

        auto currentIt{std::find(core->queue.begin(), core->queue.end(), thread)};
        if (currentIt == core->queue.end() || currentIt == core->queue.begin())
            // If the thread isn't in the queue then the new priority will be handled automatically on insertion
            return;
        if (currentIt == core->queue.begin()) {
            // Alternatively, if it's currently running then we'd just want to cause it to yield if it's priority is lower than the the thread behind it
            auto nextIt{std::next(currentIt)};
            if (nextIt != core->queue.end() && (*nextIt)->priority < thread->priority) {
                thread->SendSignal(YieldSignal);
            } else if (!thread->isPreempted && thread->priority == core->preemptionPriority) {
                // If the thread needs to be preempted due to the new priority then arm it's preemption timer
                struct itimerspec spec{.it_value = {.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(PreemptiveTimeslice).count()}};
                timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
                thread->isPreempted = true;
            }
            return;
        }

        auto targetIt{std::upper_bound(core->queue.begin(), core->queue.end(), thread->priority.load(), type::KThread::IsHigherPriority)};
        if (currentIt == targetIt)
            // If this thread's position isn't affected by the priority change then we have nothing to do
            return;

        core->queue.erase(currentIt);

        if (thread->isPreempted && thread->priority != core->preemptionPriority) {
            struct itimerspec spec{};
            timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
            thread->isPreempted = false;
        }

        targetIt = std::upper_bound(core->queue.begin(), core->queue.end(), thread->priority.load(), type::KThread::IsHigherPriority); // Iterator invalidation
        if (targetIt == core->queue.begin() && targetIt != core->queue.end()) {
            core->queue.insert(std::next(core->queue.begin()), thread);
            core->queue.front()->SendSignal(YieldSignal);
        } else {
            core->queue.insert(targetIt, thread);
        }
    }

    void Scheduler::ParkThread() {
        auto &thread{state.thread};
        std::lock_guard migrationLock(thread->coreMigrationMutex);
        RemoveThread();

        auto originalCoreId{thread->coreId};
        thread->coreId = constant::ParkedCoreId;
        for (auto &core : cores)
            if (originalCoreId != core.id && thread->affinityMask.test(core.id) && (core.queue.empty() || core.queue.front()->priority > thread->priority))
                thread->coreId = core.id;

        if (thread->coreId == constant::ParkedCoreId) {
            std::unique_lock lock(parkedMutex);
            parkedQueue.insert(std::upper_bound(parkedQueue.begin(), parkedQueue.end(), thread->priority.load(), type::KThread::IsHigherPriority), thread);
            thread->wakeCondition.wait(lock, [&]() { return parkedQueue.front() == thread && thread->coreId != constant::ParkedCoreId; });
        }

        InsertThread(thread);
    }

    void Scheduler::WakeParkedThread() {
        std::unique_lock parkedLock(parkedMutex);
        if (!parkedQueue.empty()) {
            auto &thread{state.thread};
            auto &core{cores.at(thread->coreId)};
            std::unique_lock coreLock(core.mutex);
            auto nextThread{core.queue.size() > 1 ? *std::next(core.queue.begin()) : nullptr};
            nextThread = nextThread->priority == thread->priority ? nextThread : nullptr; // If the next thread doesn't have the same priority then it won't be scheduled next
            auto parkedThread{parkedQueue.front()};

            // We need to be conservative about waking up a parked thread, it should only be done if it's priority is higher than the current thread
            // Alternatively, it should be done if it's priority is equivalent to the current thread's priority but the next thread had been scheduled prior or if there is no next thread (Current thread would be rescheduled)
            if (parkedThread->priority < thread->priority || (parkedThread->priority == thread->priority && (!nextThread || parkedThread->timesliceStart < nextThread->timesliceStart))) {
                parkedThread->coreId = thread->coreId;
                parkedLock.unlock();
                thread->wakeCondition.notify_one();
            }
        }
    }

    void Scheduler::RemoveThread() {
        auto &thread{state.thread};
        auto &core{cores.at(thread->coreId)};
        {
            std::unique_lock lock(core.mutex);
            auto it{std::find(core.queue.begin(), core.queue.end(), thread)};
            if (it != core.queue.end()) {
                it = core.queue.erase(it);
                if (it == core.queue.begin()) {
                    // We need to update the averageTimeslice accordingly, if we've been unscheduled by this
                    if (thread->timesliceStart)
                        thread->averageTimeslice = (thread->averageTimeslice / 4) + (3 * (util::GetTimeTicks() - thread->timesliceStart / 4));

                    if (it != core.queue.end())
                        (*it)->wakeCondition.notify_one(); // We need to wake the thread at the front of the queue, if we were at the front previously
                }
            }
        }

        if (thread->isPreempted) {
            struct itimerspec spec{};
            timer_settime(thread->preemptionTimer, 0, &spec, nullptr);
            thread->isPreempted = false;
        }

        YieldPending = false;
    }
}
