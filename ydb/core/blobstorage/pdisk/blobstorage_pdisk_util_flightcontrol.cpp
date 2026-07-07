#include "blobstorage_pdisk_util_flightcontrol.h"

namespace NKikimr {
namespace NPDisk {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TFlightControl
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
TFlightControl::TFlightControl(ui64 maxInFlightRequests, ui64 inFlightBytesLimit)
    : ScheduleAtomic(TScheduleAtomic{0, 0})
    , CompletionAtomic(TCompletionAtomic{1, 0})
    , EndIdx(1)
    , MaxInFlight(maxInFlightRequests)
    , MaxSize(32)
    , Mask(MaxSize - 1)
    , IsCompleteLoop(MaxSize)
{
    Y_UNUSED(inFlightBytesLimit);
    Y_VERIFY(maxInFlightRequests > 0);
}

void TFlightControl::Initialize(const TString& logPrefix) {
    PDiskLogPrefix = logPrefix;
}

// Returns 0 in case of scheduling error
// Operation Idx otherwise
// May sometimes return 0 when it already can schedule
ui64 TFlightControl::TrySchedule(ui64 size) {
    Y_UNUSED(size);

    ui64 newLastScheduledIdx = 0;
    bool isDone = false;
    while (!isDone) {
        TCompletionAtomic completionAtomic = CompletionAtomic.load(std::memory_order_relaxed);
        TScheduleAtomic scheduleAtomic = ScheduleAtomic.load(std::memory_order_relaxed);
        ui64 beginIdx = completionAtomic.BeginIdx;
        ui64 lastScheduledIdx = scheduleAtomic.LastScheduledIdx;
        if (scheduleAtomic.EnqueueCount - completionAtomic.DequeueCount >= MaxInFlight) {
            return 0;
        }
        if (lastScheduledIdx >= beginIdx) {
            bool isFull = (lastScheduledIdx - beginIdx + 1 >= MaxSize);
            if (isFull) {
                return 0;
            }
        }
        newLastScheduledIdx = lastScheduledIdx + 1;
        TScheduleAtomic newScheduleAtomic = scheduleAtomic;
        newScheduleAtomic.LastScheduledIdx = newLastScheduledIdx;
        ++newScheduleAtomic.EnqueueCount;
        isDone = ScheduleAtomic.compare_exchange_strong(scheduleAtomic, newScheduleAtomic, std::memory_order_relaxed);
    }
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
    Y_UNUSED(size);

    TCompletionAtomic completionAtomic = CompletionAtomic.load(std::memory_order_relaxed);
    ++completionAtomic.DequeueCount;
    ui64 beginIdx = completionAtomic.BeginIdx;
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

void TBytesFlightControl::Initialize(const TString& logPrefix) {
    PDiskLogPrefix = logPrefix;
}

ui64 TBytesFlightControl::TryScheduleLocked(ui64 size) {
    if (InFlightRequests >= InFlightRequestsLimit) {
        return 0;
    }
    size = std::min(size, InFlightBytesLimit);

    if (InFlightBytes + size > InFlightBytesLimit) {
        //Cerr << "reject because " << InFlightBytes << " >= " << InFlightBytesLimit << " size# " << size
        //    << " InFlightRequests# " << InFlightRequests << Endl;
        return 0;
    }

    const ui64 idx = NextScheduleIdx++;
    ++InFlightRequests;
    InFlightBytes += size;
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
    size = std::min(size, InFlightBytesLimit);

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
}

ui64 TBytesFlightControl::FirstIncompleteIdx() {
    return AtomicGet(CachedFirstIncompleteIdx);
}

} // NPDisk
} // NKikimr
