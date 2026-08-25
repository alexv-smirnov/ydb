#include "blobstorage_pdisk_util_flightcontrol.h"

namespace NKikimr {
namespace NPDisk {

#define PDISK_FLIGHTCONTROL_TRACE(actorSystem, stream) \
    do { \
        if (actorSystem) { \
            YDB_LOG_TRACE_CTX_COMP(*(actorSystem), NKikimrServices::BS_PDISK, stream); \
        } \
    } while (false)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TFlightControl
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
TFlightControl::TFlightControl(ui64 maxInFlightRequests, ui64 inFlightBytesLimit)
    : ScheduleAtomic(TScheduleAtomic{0, 0})
    , CompletionAtomic(TCompletionAtomic{1, 0})
    , EndIdx(1)
    , MaxInFlight(maxInFlightRequests)
    , MaxSize(Max<ui64>(32, maxInFlightRequests))
    , Mask(MaxSize - 1)
    , IsCompleteLoop(MaxSize)
{
    Y_UNUSED(inFlightBytesLimit);
    Y_VERIFY(maxInFlightRequests > 0);
    Y_VERIFY((MaxSize & (MaxSize - 1)) == 0);
}

void TFlightControl::Initialize(const TString& logPrefix, NActors::TActorSystem* actorSystem) {
    PDiskLogPrefix = logPrefix;
    ActorSystem = actorSystem;

    TScheduleAtomic scheduleAtomic = ScheduleAtomic.load(std::memory_order_relaxed);
    TCompletionAtomic completionAtomic = CompletionAtomic.load(std::memory_order_relaxed);
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::Initialize"
            << " LastScheduledIdx# " << scheduleAtomic.LastScheduledIdx
            << " EnqueueCount# " << scheduleAtomic.EnqueueCount
            << " BeginIdx# " << completionAtomic.BeginIdx
            << " DequeueCount# " << completionAtomic.DequeueCount
            << " EndIdx# " << EndIdx
            << " MaxInFlight# " << MaxInFlight
            << " MaxSize# " << MaxSize
            << " Mask# " << Mask);
}

// Returns 0 in case of scheduling error
// Operation Idx otherwise
// May sometimes return 0 when it already can schedule
ui64 TFlightControl::TrySchedule(ui64 size) {
    ui64 newLastScheduledIdx = 0;
    bool isDone = false;
    bool entryLogged = false;
    TCompletionAtomic completionAtomic;
    TScheduleAtomic newScheduleAtomic;
    while (!isDone) {
        completionAtomic = CompletionAtomic.load(std::memory_order_relaxed);
        TScheduleAtomic scheduleAtomic = ScheduleAtomic.load(std::memory_order_relaxed);
        ui64 beginIdx = completionAtomic.BeginIdx;
        ui64 lastScheduledIdx = scheduleAtomic.LastScheduledIdx;
        if (!entryLogged) {
            entryLogged = true;
            PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::TrySchedule entry"
                    << " Size# " << size
                    << " LastScheduledIdx# " << lastScheduledIdx
                    << " EnqueueCount# " << scheduleAtomic.EnqueueCount
                    << " BeginIdx# " << beginIdx
                    << " DequeueCount# " << completionAtomic.DequeueCount
                    << " MaxInFlight# " << MaxInFlight
                    << " MaxSize# " << MaxSize);
        }
        if (scheduleAtomic.EnqueueCount - completionAtomic.DequeueCount >= MaxInFlight) {
            PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::TrySchedule exit"
                    << " Result# 0"
                    << " Reason# MaxInFlight"
                    << " LastScheduledIdx# " << lastScheduledIdx
                    << " EnqueueCount# " << scheduleAtomic.EnqueueCount
                    << " BeginIdx# " << beginIdx
                    << " DequeueCount# " << completionAtomic.DequeueCount
                    << " InFlight# " << scheduleAtomic.EnqueueCount - completionAtomic.DequeueCount
                    << " MaxInFlight# " << MaxInFlight);
            return 0;
        }
        if (lastScheduledIdx >= beginIdx) {
            bool isFull = (lastScheduledIdx - beginIdx + 1 >= MaxSize);
            if (isFull) {
                PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::TrySchedule exit"
                        << " Result# 0"
                        << " Reason# MaxSize"
                        << " LastScheduledIdx# " << lastScheduledIdx
                        << " EnqueueCount# " << scheduleAtomic.EnqueueCount
                        << " BeginIdx# " << beginIdx
                        << " DequeueCount# " << completionAtomic.DequeueCount
                        << " Distance# " << lastScheduledIdx - beginIdx + 1
                        << " MaxSize# " << MaxSize);
                return 0;
            }
        }
        newLastScheduledIdx = lastScheduledIdx + 1;
        newScheduleAtomic = scheduleAtomic;
        newScheduleAtomic.LastScheduledIdx = newLastScheduledIdx;
        ++newScheduleAtomic.EnqueueCount;
        isDone = ScheduleAtomic.compare_exchange_strong(scheduleAtomic, newScheduleAtomic, std::memory_order_relaxed);
    }
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::TrySchedule exit"
            << " Result# " << newLastScheduledIdx
            << " LastScheduledIdx# " << newScheduleAtomic.LastScheduledIdx
            << " EnqueueCount# " << newScheduleAtomic.EnqueueCount
            << " BeginIdx# " << completionAtomic.BeginIdx
            << " DequeueCount# " << completionAtomic.DequeueCount
            << " InFlight# " << newScheduleAtomic.EnqueueCount - completionAtomic.DequeueCount);
    return newLastScheduledIdx;
}

// Blocking Schedule method
ui64 TFlightControl::Schedule(double& blockedMs, ui64 size) {
    NHPTimer::STime beginTime = 0;
    while (true) {
        ui64 idx = TrySchedule(size);
        if (idx) {
            return idx;
        } else {
            TGuard<TMutex> guard(ScheduleMutex);
            idx = TrySchedule(size);
            if (idx) {
                return idx;
            }
            if (beginTime == 0) {
                beginTime = HPNow();
            }
            ScheduleCondVar.WaitI(ScheduleMutex);
            blockedMs = HPMilliSecondsFloat(HPNow() - beginTime);
        }
    }
}

void TFlightControl::WakeUp() {
    TGuard<TMutex> guard(ScheduleMutex);
    ScheduleCondVar.Signal();
}

void TFlightControl::MarkComplete(ui64 idx, ui64 size) {
    TCompletionAtomic completionAtomic = CompletionAtomic.load(std::memory_order_relaxed);
    TScheduleAtomic scheduleAtomic = ScheduleAtomic.load(std::memory_order_relaxed);
    ui64 beginIdx = completionAtomic.BeginIdx;
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::MarkComplete entry"
            << " Idx# " << idx
            << " Size# " << size
            << " LastScheduledIdx# " << scheduleAtomic.LastScheduledIdx
            << " EnqueueCount# " << scheduleAtomic.EnqueueCount
            << " BeginIdx# " << beginIdx
            << " DequeueCount# " << completionAtomic.DequeueCount
            << " EndIdx# " << EndIdx
            << " MaxInFlight# " << MaxInFlight
            << " MaxSize# " << MaxSize);
    ++completionAtomic.DequeueCount;
    Y_VERIFY_S(idx >= beginIdx, PDiskLogPrefix);
    Y_VERIFY_S(idx < beginIdx + MaxSize, PDiskLogPrefix);
    if (idx == beginIdx) {
        // It's the first item we are waiting for
        if (beginIdx == EndIdx) {
            // The loop was empty, just move both begin and end
            completionAtomic.BeginIdx = beginIdx + 1;
            CompletionAtomic.store(completionAtomic, std::memory_order_relaxed);
            ++EndIdx;
            WakeUp();
            PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::MarkComplete exit"
                    << " Idx# " << idx
                    << " Branch# FirstAndEmpty"
                    << " BeginIdx# " << completionAtomic.BeginIdx
                    << " DequeueCount# " << completionAtomic.DequeueCount
                    << " EndIdx# " << EndIdx);
            return;
        }
        // The loop was not empty, move begin forward once, then skip all the complete items
        ++beginIdx;
        while (beginIdx < EndIdx && IsCompleteLoop[beginIdx & Mask]) {
            ++beginIdx;
        }
        completionAtomic.BeginIdx = beginIdx;
        CompletionAtomic.store(completionAtomic, std::memory_order_relaxed);
        WakeUp();
        PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::MarkComplete exit"
                << " Idx# " << idx
                << " Branch# First"
                << " BeginIdx# " << completionAtomic.BeginIdx
                << " DequeueCount# " << completionAtomic.DequeueCount
                << " EndIdx# " << EndIdx);
        return;
    }
    // It's not the first item
    if (idx >= EndIdx) {
        for (ui64 i = EndIdx; i < idx; ++i) {
            IsCompleteLoop[i & Mask] = false;
        }
        EndIdx = idx + 1;
    }
    IsCompleteLoop[idx & Mask] = true;
    CompletionAtomic.store(completionAtomic, std::memory_order_relaxed);

    if (MaxSize > MaxInFlight) {
        WakeUp();
    }
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TFlightControl::MarkComplete exit"
            << " Idx# " << idx
            << " Branch# OutOfOrder"
            << " BeginIdx# " << completionAtomic.BeginIdx
            << " DequeueCount# " << completionAtomic.DequeueCount
            << " EndIdx# " << EndIdx);
}

ui64 TFlightControl::FirstIncompleteIdx() {
    return CompletionAtomic.load(std::memory_order_relaxed).BeginIdx;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TBytesFlightControl
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
TBytesFlightControl::TBytesFlightControl(ui64 inFlightRequestsLimit, ui64 inFlightBytesLimit)
    : InFlightRequestsLimit(inFlightRequestsLimit)
    , InFlightBytesLimit(inFlightBytesLimit)
    , CachedFirstIncompleteIdx(1)
    , NextScheduleIdx(1)
    , InFlightRequests(0)
    , InFlightBytes(0)
    , FirstIncompleteIdxValue(1)
{
    Y_VERIFY(inFlightRequestsLimit > 0);
    Y_VERIFY(inFlightBytesLimit > 0);
}

void TBytesFlightControl::Initialize(const TString& logPrefix, NActors::TActorSystem* actorSystem) {
    PDiskLogPrefix = logPrefix;
    ActorSystem = actorSystem;

    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::Initialize"
            << " InFlightRequestsLimit# " << InFlightRequestsLimit
            << " InFlightBytesLimit# " << InFlightBytesLimit
            << " CachedFirstIncompleteIdx# " << AtomicGet(CachedFirstIncompleteIdx)
            << " NextScheduleIdx# " << NextScheduleIdx
            << " InFlightRequests# " << InFlightRequests
            << " InFlightBytes# " << InFlightBytes
            << " FirstIncompleteIdxValue# " << FirstIncompleteIdxValue);
}

ui64 TBytesFlightControl::TryScheduleLocked(ui64 size) {
    const ui64 requestedSize = size;
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::TrySchedule entry"
            << " Size# " << requestedSize
            << " NextScheduleIdx# " << NextScheduleIdx
            << " InFlightRequests# " << InFlightRequests
            << " InFlightBytes# " << InFlightBytes
            << " InFlightRequestsLimit# " << InFlightRequestsLimit
            << " InFlightBytesLimit# " << InFlightBytesLimit
            << " FirstIncompleteIdxValue# " << FirstIncompleteIdxValue);
    if (InFlightRequests >= InFlightRequestsLimit) {
        PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::TrySchedule exit"
                << " Result# 0"
                << " Reason# InFlightRequestsLimit"
                << " Size# " << requestedSize
                << " InFlightRequests# " << InFlightRequests
                << " InFlightRequestsLimit# " << InFlightRequestsLimit);
        return 0;
    }
    size = std::min(size, InFlightBytesLimit);

    if (InFlightBytes + size > InFlightBytesLimit) {
        //Cerr << "reject because " << InFlightBytes << " >= " << InFlightBytesLimit << " size# " << size
        //    << " InFlightRequests# " << InFlightRequests << Endl;
        PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::TrySchedule exit"
                << " Result# 0"
                << " Reason# InFlightBytesLimit"
                << " RequestedSize# " << requestedSize
                << " EffectiveSize# " << size
                << " InFlightBytes# " << InFlightBytes
                << " InFlightBytesLimit# " << InFlightBytesLimit);
        return 0;
    }

    const ui64 idx = NextScheduleIdx++;
    ++InFlightRequests;
    InFlightBytes += size;
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::TrySchedule exit"
            << " Result# " << idx
            << " RequestedSize# " << requestedSize
            << " EffectiveSize# " << size
            << " NextScheduleIdx# " << NextScheduleIdx
            << " InFlightRequests# " << InFlightRequests
            << " InFlightBytes# " << InFlightBytes);
    return idx;
}

// Returns 0 in case of scheduling error
// Operation Idx otherwise
// May sometimes return 0 when it already can schedule
ui64 TBytesFlightControl::TrySchedule(ui64 size) {
    TGuard<TMutex> guard(ScheduleMutex);
    return TryScheduleLocked(size);
}

// Blocking Schedule method
ui64 TBytesFlightControl::Schedule(double& blockedMs, ui64 size) {
    NHPTimer::STime beginTime = 0;
    TGuard<TMutex> guard(ScheduleMutex);
    while (true) {
        if (ui64 idx = TryScheduleLocked(size)) {
            return idx;
        }
        if (beginTime == 0) {
            beginTime = HPNow();
        }
        ScheduleCondVar.WaitI(ScheduleMutex);
        blockedMs = HPMilliSecondsFloat(HPNow() - beginTime);
    }
}

void TBytesFlightControl::MarkComplete(ui64 idx, ui64 size) {
    TGuard<TMutex> guard(ScheduleMutex);
    const ui64 requestedSize = size;
    size = std::min(size, InFlightBytesLimit);
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::MarkComplete entry"
            << " Idx# " << idx
            << " RequestedSize# " << requestedSize
            << " EffectiveSize# " << size
            << " NextScheduleIdx# " << NextScheduleIdx
            << " InFlightRequests# " << InFlightRequests
            << " InFlightBytes# " << InFlightBytes
            << " FirstIncompleteIdxValue# " << FirstIncompleteIdxValue
            << " CompletedIdxSize# " << CompletedIdx.size());

    Y_VERIFY_S(idx >= FirstIncompleteIdxValue, PDiskLogPrefix << " idx# " << idx
            << " FirstIncompleteIdxValue# " << FirstIncompleteIdxValue);
    Y_VERIFY_S(InFlightRequests > 0, PDiskLogPrefix);
    Y_VERIFY_S(InFlightBytes >= size, PDiskLogPrefix << " InFlightBytes# " << InFlightBytes
            << " size# " << size);

    --InFlightRequests;
    InFlightBytes -= size;

    CompletedIdx.push(idx);
    while (!CompletedIdx.empty() && CompletedIdx.top() == FirstIncompleteIdxValue) {
        ++FirstIncompleteIdxValue;
        CompletedIdx.pop();
    }

    AtomicSet(CachedFirstIncompleteIdx, FirstIncompleteIdxValue);
    ScheduleCondVar.Signal();
    PDISK_FLIGHTCONTROL_TRACE(ActorSystem, PDiskLogPrefix << "TBytesFlightControl::MarkComplete exit"
            << " Idx# " << idx
            << " NextScheduleIdx# " << NextScheduleIdx
            << " InFlightRequests# " << InFlightRequests
            << " InFlightBytes# " << InFlightBytes
            << " FirstIncompleteIdxValue# " << FirstIncompleteIdxValue
            << " CompletedIdxSize# " << CompletedIdx.size());
}

ui64 TBytesFlightControl::FirstIncompleteIdx() {
    return AtomicGet(CachedFirstIncompleteIdx);
}

} // NPDisk
} // NKikimr

#undef PDISK_FLIGHTCONTROL_TRACE
