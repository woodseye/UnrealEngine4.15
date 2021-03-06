// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MetalQuery.cpp: Metal query RHI implementation.
=============================================================================*/

#include "MetalRHIPrivate.h"

FMetalQueryBuffer::FMetalQueryBuffer(FMetalContext* InContext, id<MTLBuffer> InBuffer)
: Pool(InContext->GetQueryBufferPool())
, Buffer(InBuffer)
, WriteOffset(0)
{
}

FMetalQueryBuffer::~FMetalQueryBuffer()
{
	if(Buffer && GIsRHIInitialized)
    {
		TSharedPtr<FMetalQueryBufferPool, ESPMode::ThreadSafe> BufferPool = Pool.Pin();
		if (BufferPool.IsValid())
		{
			BufferPool->ReleaseQueryBuffer(Buffer);
		}
		else
		{
			[Buffer release];
		}
    }
}

uint64 FMetalQueryBuffer::GetResult(uint32 Offset)
{
    check(Buffer);
	uint64 Result = 0;
	@autoreleasepool
	{
		Result = *((uint64 const*)(((uint8*)[Buffer contents]) + Offset));
	}
    return Result;
}

bool FMetalCommandBufferFence::Wait(uint64 Millis)
{
	@autoreleasepool
	{
		TSharedPtr<MTLCommandBufferRef, ESPMode::ThreadSafe> CommandBuffer = CommandBufferRef.Pin();
		if (CommandBuffer.IsValid())
		{
			bool bFinished = false;
			if(!bFinished)
			{
				check([CommandBuffer->CommandBuffer status] >= MTLCommandBufferStatusCommitted && [CommandBuffer->CommandBuffer status] <= MTLCommandBufferStatusError);
				dispatch_time_t Timeout = Millis > 0 ? dispatch_time(DISPATCH_TIME_NOW, Millis * 1000000llu) : DISPATCH_TIME_NOW;
				bFinished = (dispatch_semaphore_wait(CommandBuffer->Semaphore, Timeout) == 0);
				if (bFinished)
				{
					[CommandBuffer->CommandBuffer waitUntilCompleted];
				}
				check([CommandBuffer->CommandBuffer status] >= MTLCommandBufferStatusCommitted && [CommandBuffer->CommandBuffer status] <= MTLCommandBufferStatusError);
			}
			if([CommandBuffer->CommandBuffer status] == MTLCommandBufferStatusError)
			{
				FMetalCommandList::HandleMetalCommandBufferFailure(CommandBuffer->CommandBuffer);
			}
			FPlatformMisc::MemoryBarrier();
			return bFinished && ([CommandBuffer->CommandBuffer status] >= MTLCommandBufferStatusCompleted);
		}
		else
		{
			return true;
		}
	}
}
	
bool FMetalQueryResult::Wait(uint64 Millis)
{
	if(!bCompleted)
	{
		bCompleted = CommandBufferFence.Wait(Millis);
		
		return bCompleted;
	}
	return true;
}

uint64 FMetalQueryResult::GetResult()
{
	if(IsValidRef(SourceBuffer))
	{
		return SourceBuffer->GetResult(Offset);
	}
	return 0;
}

FMetalRenderQuery::FMetalRenderQuery(ERenderQueryType InQueryType)
: Type(InQueryType)
{
	Result = 0;
	bAvailable = false;
	Buffer.Offset = 0;
	Buffer.bCompleted = false;
}

FMetalRenderQuery::~FMetalRenderQuery()
{
	Buffer.SourceBuffer.SafeRelease();
	Buffer.Offset = 0;
}

void FMetalRenderQuery::Begin(FMetalContext* Context)
{
	Buffer.SourceBuffer.SafeRelease();
	Buffer.Offset = 0;
	
	Result = 0;
	bAvailable = false;
	
	if(Type == RQT_Occlusion)
	{
		// allocate our space in the current buffer
		Context->GetQueryBufferPool()->Allocate(Buffer);
		Buffer.bCompleted = false;
		
		if ((GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM4) && GetMetalDeviceContext().SupportsFeature(EMetalFeaturesCountingQueries))
		{
			Context->GetCurrentState().SetVisibilityResultMode(MTLVisibilityResultModeCounting, Buffer.Offset);
		}
		else
		{
			Context->GetCurrentState().SetVisibilityResultMode(MTLVisibilityResultModeBoolean, Buffer.Offset);
		}
	}
}

void FMetalRenderQuery::End(FMetalContext* Context)
{
	switch(Type)
	{
		case RQT_Occlusion:
		{
			// switch back to non-occlusion rendering
			Context->GetCurrentState().SetVisibilityResultMode(MTLVisibilityResultModeDisabled, 0);
			Context->InsertCommandBufferFence(Buffer.CommandBufferFence);
			break;
		}
		case RQT_AbsoluteTime:
		{
			// Reset the result availability state
			Result = 0;
			bAvailable = false;
			Buffer.bCompleted = false;
			Buffer.CommandBufferFence.CommandBufferRef.Reset();
			
			// Insert the fence to wait on the current command buffer
			Context->InsertCommandBufferFence(Buffer.CommandBufferFence, ^(id<MTLCommandBuffer> _Nonnull)
			{
				Result = (FPlatformTime::ToMilliseconds64(mach_absolute_time()) * 1000.0);
			});
			
			// Submit the current command buffer, marking this is as a break of a logical command buffer for render restart purposes
			// This is necessary because we use command-buffer completion to emulate timer queries as Metal has no such API
			Context->SubmitCommandsHint(EMetalSubmitFlagsCreateCommandBuffer|EMetalSubmitFlagsBreakCommandBuffer);
			break;
		}
		default:
		{
			check(0);
			break;
		}
	}
}

FRenderQueryRHIRef FMetalDynamicRHI::RHICreateRenderQuery_RenderThread(class FRHICommandListImmediate& RHICmdList, ERenderQueryType QueryType)
{
	return GDynamicRHI->RHICreateRenderQuery(QueryType);
}

FRenderQueryRHIRef FMetalDynamicRHI::RHICreateRenderQuery(ERenderQueryType QueryType)
{
	FRenderQueryRHIRef Query;
	// AMD have subtleties to their completion handler routines that mean we don't seem able to reliably wait on command-buffers
	// until after a drawable present...
	static bool const bSupportsTimeQueries = GetMetalDeviceContext().GetCommandQueue().SupportsFeature(EMetalFeaturesAbsoluteTimeQueries);
	if (QueryType != RQT_AbsoluteTime || bSupportsTimeQueries)
	{
		Query = new FMetalRenderQuery(QueryType);
	}
	return Query;
}

bool FMetalDynamicRHI::RHIGetRenderQueryResult(FRenderQueryRHIParamRef QueryRHI,uint64& OutNumPixels,bool bWait)
{
	check(IsInRenderingThread());
	FMetalRenderQuery* Query = ResourceCast(QueryRHI);
	
    if(!Query->bAvailable)
    {
		SCOPE_CYCLE_COUNTER( STAT_RenderQueryResultTime );
		
		bool bOK = false;
		
		// timer queries are used for Benchmarks which can stall a bit more
		uint64 WaitMS = (Query->Type == RQT_AbsoluteTime) ? 2000 : 500;
		if (bWait)
		{
			uint32 IdleStart = FPlatformTime::Cycles();
		
			bOK = Query->Buffer.Wait(WaitMS);
			
			GRenderThreadIdle[ERenderThreadIdleTypes::WaitingForGPUQuery] += FPlatformTime::Cycles() - IdleStart;
			GRenderThreadNumIdle[ERenderThreadIdleTypes::WaitingForGPUQuery]++;
			
			// Never wait for a failed signal again.
			Query->bAvailable = Query->Buffer.bCompleted;
		}
		else
		{
			bOK = Query->Buffer.Wait(0);
		}
		
        if (bOK == false)
        {
			OutNumPixels = 0;
			UE_CLOG(bWait, LogMetal, Display, TEXT("Timed out while waiting for GPU to catch up. (%llu ms)"), WaitMS);
			return false;
        }
		
		if(Query->Type == RQT_Occlusion)
		{
			Query->Result = Query->Buffer.GetResult();
		}
		Query->Buffer.SourceBuffer.SafeRelease();
    }

	// at this point, we are ready to read the value!
	OutNumPixels = Query->Result;
    return true;
}

// Occlusion/Timer queries.
void FMetalRHICommandContext::RHIBeginRenderQuery(FRenderQueryRHIParamRef QueryRHI)
{
	FMetalRenderQuery* Query = ResourceCast(QueryRHI);
	
	Query->Begin(Context);
}

void FMetalRHICommandContext::RHIEndRenderQuery(FRenderQueryRHIParamRef QueryRHI)
{
	FMetalRenderQuery* Query = ResourceCast(QueryRHI);
	
	Query->End(Context);
}

void FMetalRHICommandContext::RHIBeginOcclusionQueryBatch()
{
}

void FMetalRHICommandContext::RHIEndOcclusionQueryBatch()
{
}
