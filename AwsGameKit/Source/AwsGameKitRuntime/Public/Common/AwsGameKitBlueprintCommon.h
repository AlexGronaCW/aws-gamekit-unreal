// Copyright 2022 Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Models/AwsGameKitCommonModels.h"

// GameKit
#include "Core/AwsGameKitErrors.h"

// Unreal
#include "Async/Async.h"
#include "Containers/Queue.h"
#include "Engine/LatentActionManager.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "LatentActions.h"
#include "Misc/Optional.h"

UENUM()
enum class EAwsGameKitSuccessOrFailureExecutionPin : uint8
{
    OnSuccess, OnFailure
};

template <typename ResultType>
struct TAwsGameKitInternalActionState
{
    FAwsGameKitOperationResult Err;
    ResultType Results;
    TOptional<TQueue<ResultType>> PartialResultsQueue;
};

template <typename ResultType = FNoopStruct>
using TAwsGameKitInternalActionStatePtr = TSharedPtr<TAwsGameKitInternalActionState<ResultType>, ESPMode::ThreadSafe>;

template <typename VariableType>
static FORCENOINLINE VariableType& InternalAwsGameKitThreadedActionSafeOutputRef(const FLatentActionInfo& LatentInfo, VariableType& OutRef)
{
    // Unspeakable evil: in some cases, the frame slots for disconnected (ignored) output pins can be compiled away
    // and the output references are pointing at local C++ stack variables instead of the persistent blueprint frame.
    // We need to detect this and avoid writing to these stack addresses "later" when our latent function completes.
    static_assert(USE_UBER_GRAPH_PERSISTENT_FRAME, "internal AwsGameKit implementation requires persistent frames");
    UFunction* Function = LatentInfo.CallbackTarget->FindFunction(LatentInfo.ExecutionFunction);
    void* PersistentFrame = Function->GetOuterUClassUnchecked()->GetPersistentUberGraphFrame(LatentInfo.CallbackTarget, Function);
    int32 PersistentFrameSize = StaticCast<UBlueprintGeneratedClass*>(Function->GetOuterUClassUnchecked())->UberGraphFunction->GetStructureSize();

    // 256 MB seems like a good sanity-check limit for a single function frame
    constexpr int32 ExtremelyLargeBPFrameSize = 0x10000000;

    // If this check fires, something is very wrong with our internal assumptions about how BP function calls work.
    check(PersistentFrame && PersistentFrameSize > 0 && PersistentFrameSize < ExtremelyLargeBPFrameSize);

    if ((char*)&OutRef >= (char*)PersistentFrame && (char*)&OutRef < (char*)PersistentFrame + PersistentFrameSize)
    {
        // OutVar points into persistent frame memory, we can safely defer writes to this address.
        return OutRef;
    }
    else
    {
        // OutVar is a stack variable from the exec* generated function or another C++ wrapper
        // because the blueprint output property was unused and compiled away on this platform.
#if DO_CHECK
        // If this check fires, either something is wrong with our assumptions about how BP function calls work,
        // or else an AwsGameKit wrapper function has somehow exceeded a very large stack size even though the
        // wrapper shouldn't be doing any work; check the callstack and fix the stack usage of the parent function.
        // (Note: this check assumes that stack grows downward towards lower addresses on all target platforms.)
        volatile char stackobj = 1;
        constexpr size_t NearbyStackAddressLimit = 16384u;
        check((size_t)((char*)&OutRef - &stackobj) < NearbyStackAddressLimit);
#endif
        // It is OK to return a static variable here since BP execution is single-threaded;
        // even if the type is complex, we won't have any issues with concurrent writes.
        static VariableType StaticJunkVarForIgnoredOutput;
        return StaticJunkVarForIgnoredOutput;
    }
}

template <typename RequestType, typename ResultType, typename PartialResultsDelegateType = FNoopStruct>
class AWSGAMEKITRUNTIME_API TAwsGameKitInternalThreadedAction : public FPendingLatentAction
{
public:
    // The output reference captures may look wildly unsafe, but when Blueprint calls latent actions with output parameters,
    // the parameters have stable heap addresses which are owned by the blueprint virtual machine. Note, the blueprint VM
    // may be destroyed during app shutdown (or other UObject cleanup) before the async action has completed, so the async
    // code MUST not reference the output variables directly. We proxy the output through a heap-allocated shared object.
    TAwsGameKitInternalThreadedAction(const FLatentActionInfo& LatentInfoParam, const RequestType& RequestParam, EAwsGameKitSuccessOrFailureExecutionPin& SuccessOrFailureParam, FAwsGameKitOperationResult& StatusParam, ResultType& ResultsParam, const PartialResultsDelegateType& PartialResultsDelegateParam)
        : ThreadedState(new TAwsGameKitInternalActionState<ResultType>),
        LatentInfo(LatentInfoParam),
        InRequest(RequestParam),
        OutSuccessOrFailure(InternalAwsGameKitThreadedActionSafeOutputRef(LatentInfoParam, SuccessOrFailureParam)),
        OutResults(InternalAwsGameKitThreadedActionSafeOutputRef(LatentInfoParam, ResultsParam)),
        OutStatus(InternalAwsGameKitThreadedActionSafeOutputRef(LatentInfoParam, StatusParam)),
        PartialResultsDelegate(PartialResultsDelegateParam)
    {
        InitializePartialResultQueue(PartialResultsDelegate);
    }

    // Note: async threaded work may outlive this Action object or the entire Blueprint VM,
    // so any data being passed back to this Action needs to bounce via a shared heap object
    TAwsGameKitInternalActionStatePtr<ResultType> ThreadedState;

    // LaunchThreadedWork MUST be called immediately; the lambda should capture + fill ThreadedState,
    // and should stream partial result sets into ThreadedState->PartialResultsQueue if it is valid.
    // (If ThreadedState->PartialResultsQueue is not a valid object, it means that no partial-results
    // delegate was provided and there is no need to stream partial results via threadsafe queueing.)
    template <typename LambdaType>
    void LaunchThreadedWork(LambdaType&& Lambda)
    {
        ThreadedResult = Async(EAsyncExecution::Thread, MoveTemp(Lambda));
    }

private:
    // This override function is regularly called by the latent action manager
    virtual void UpdateOperation(FLatentResponse& Response) override
    {
        check(ThreadedResult.IsValid()); // If this check fires, it means Launch was not called
        if (ThreadedResult.IsReady())
        {
            DispatchPartialResults(PartialResultsDelegate, true);
            OutResults = MoveTemp(ThreadedState->Results);
            OutStatus = ThreadedState->Err;
            OutSuccessOrFailure = ThreadedState->Err.Status == GameKit::GAMEKIT_SUCCESS ? EAwsGameKitSuccessOrFailureExecutionPin::OnSuccess : EAwsGameKitSuccessOrFailureExecutionPin::OnFailure;
            Response.FinishAndTriggerIf(true, LatentInfo.ExecutionFunction, LatentInfo.Linkage, LatentInfo.CallbackTarget);
        }
        else
        {
            DispatchPartialResults(PartialResultsDelegate, false);
        }
    }


    // Note, partial-results logic is wrapped into overloaded helper functions to avoid repeating too
    // much Action logic for specialization of cases where we we don't have partial-result delegates

    static void InitializePartialResultQueue(FNoopStruct&)
    {}

    static void DispatchPartialResults(FNoopStruct&, bool)
    {}

    template <typename T>
    void InitializePartialResultQueue(T& Delegate)
    {
        if (Delegate.IsBound())
        {
            ThreadedState->PartialResultsQueue.Emplace();
        }
    }

    template <typename T>
    void DispatchPartialResults(T& Delegate, bool bThreadComplete)
    {
        if (!Delegate.IsBound())
            return;

        check(ThreadedState->PartialResultsQueue);

        bool bInvokedWithFinal = false;

        ResultType TempResults;
        while (ThreadedState->PartialResultsQueue->Dequeue(TempResults))
        {
            // Note: if !bThreadComplete, IsEmpty is unsafe since thread may still be producing
            bool bFinalInvoke = bThreadComplete && ThreadedState->PartialResultsQueue->IsEmpty();
            bInvokedWithFinal |= bFinalInvoke;
            Delegate.Execute(InRequest, TempResults, bFinalInvoke);
        }

        if (bThreadComplete && !bInvokedWithFinal)
        {
            ResultType EmptyResults;
            Delegate.Execute(InRequest, EmptyResults, true);
        }
    }

private:
    FLatentActionInfo LatentInfo;
    RequestType InRequest;
    EAwsGameKitSuccessOrFailureExecutionPin& OutSuccessOrFailure;
    ResultType& OutResults;
    FAwsGameKitOperationResult& OutStatus;
    PartialResultsDelegateType PartialResultsDelegate;
    TFuture<void> ThreadedResult;
};

template <typename RequestType, typename ResultType, typename StreamingDelegateType = FNoopStruct>
auto InternalMakeAwsGameKitThreadedAction(TAwsGameKitInternalActionStatePtr<ResultType>& State,
    const UObject* WorldContextObject, const FLatentActionInfo& LatentInfo,
    const RequestType& Request, EAwsGameKitSuccessOrFailureExecutionPin& SuccessOrFailure, FAwsGameKitOperationResult& Status,
    ResultType& Results, const StreamingDelegateType& Delegate = StreamingDelegateType())
    -> TAwsGameKitInternalThreadedAction<RequestType, ResultType, StreamingDelegateType>*
{
    typedef TAwsGameKitInternalThreadedAction<RequestType, ResultType, StreamingDelegateType> ActionType;

    FLatentActionManager& LatentActionManager = WorldContextObject->GetWorld()->GetLatentActionManager();

    ActionType* Action = new ActionType(LatentInfo, Request, SuccessOrFailure, Status, Results, Delegate);
    LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID, Action);
    State = Action->ThreadedState;
    return Action;
}

template <typename RequestType>
auto InternalMakeAwsGameKitThreadedAction(TAwsGameKitInternalActionStatePtr<>& State,
    const UObject* WorldContextObject, const FLatentActionInfo& LatentInfo,
    const RequestType& Request, EAwsGameKitSuccessOrFailureExecutionPin& SuccessOrFailure, FAwsGameKitOperationResult& Status)
    -> TAwsGameKitInternalThreadedAction<RequestType, FNoopStruct, FNoopStruct>*
{
    FNoopStruct Result; // Stack output will be remapped to global static junk variable by InternalAwsGameKitThreadedActionSafeOutputRef
    return InternalMakeAwsGameKitThreadedAction(State, WorldContextObject, LatentInfo, Request, SuccessOrFailure, Status, Result, FNoopStruct());
}
