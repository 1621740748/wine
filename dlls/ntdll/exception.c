/*
 * NT exception handling routines
 * 
 * Copyright 1999 Turchanov Sergey
 * Copyright 1999 Alexandre Julliard
 */

#include "debugtools.h"
#include "winnt.h"
#include "ntddk.h"
#include "except.h"
#include "stackframe.h"

DEFAULT_DEBUG_CHANNEL(seh)

/* Exception record for handling exceptions happening inside exception handlers */
typedef struct
{
    EXCEPTION_FRAME frame;
    EXCEPTION_FRAME *prevFrame;
} EXC_NESTED_FRAME;


/*******************************************************************
 *         EXC_RaiseHandler
 *
 * Handler for exceptions happening inside a handler.
 */
static DWORD CALLBACK EXC_RaiseHandler( EXCEPTION_RECORD *rec, EXCEPTION_FRAME *frame,
                                        CONTEXT *context, EXCEPTION_FRAME **dispatcher )
{
    if (rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND))
        return ExceptionContinueSearch;
    /* We shouldn't get here so we store faulty frame in dispatcher */
    *dispatcher = ((EXC_NESTED_FRAME*)frame)->prevFrame;
    return ExceptionNestedException;
}


/*******************************************************************
 *         EXC_UnwindHandler
 *
 * Handler for exceptions happening inside an unwind handler.
 */
static DWORD CALLBACK EXC_UnwindHandler( EXCEPTION_RECORD *rec, EXCEPTION_FRAME *frame,
                                         CONTEXT *context, EXCEPTION_FRAME **dispatcher )
{
    if (!(rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)))
        return ExceptionContinueSearch;
    /* We shouldn't get here so we store faulty frame in dispatcher */
    *dispatcher = ((EXC_NESTED_FRAME*)frame)->prevFrame;
    return ExceptionCollidedUnwind;
}


/*******************************************************************
 *         EXC_CallHandler
 *
 * Call an exception handler, setting up an exception frame to catch exceptions
 * happening during the handler execution.
 */
static DWORD EXC_CallHandler( PEXCEPTION_HANDLER handler, PEXCEPTION_HANDLER nested_handler,
                              EXCEPTION_RECORD *record, EXCEPTION_FRAME *frame,
                              CONTEXT *context, EXCEPTION_FRAME **dispatcher )
{
    EXC_NESTED_FRAME newframe;
    DWORD ret;

    newframe.frame.Handler = nested_handler;
    newframe.prevFrame     = frame;
    EXC_push_frame( &newframe.frame );
    TRACE( "calling handler at %p\n", handler );
    ret = handler( record, frame, context, dispatcher );
    TRACE( "handler returned %lx\n", ret );
    EXC_pop_frame( &newframe.frame );
    return ret;
}


/*******************************************************************
 *         EXC_DefaultHandling
 *
 * Default handling for exceptions. Called when we didn't find a suitable handler.
 */
static void EXC_DefaultHandling( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    if (rec->ExceptionFlags & EH_STACK_INVALID)
        ERR("Exception frame is not in stack limits => unable to dispatch exception.\n");
    else if (rec->ExceptionCode == EXCEPTION_NONCONTINUABLE_EXCEPTION)
        ERR("Process attempted to continue execution after noncontinuable exception.\n");
    else
        ERR("Unhandled exception code %lx flags %lx addr %p\n",
            rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress );
    /* Should I add here a back trace ? */
    TerminateProcess( GetCurrentProcess(), 1 );
}


/*******************************************************************
 *         EXC_RaiseException
 *
 * Implementation of NtRaiseException.
 */
static void EXC_RaiseException( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    PEXCEPTION_FRAME frame, dispatch, nested_frame;
    EXCEPTION_RECORD newrec;
    DWORD res;

    frame = NtCurrentTeb()->except;
    nested_frame = NULL;
    while (frame != (PEXCEPTION_FRAME)0xFFFFFFFF)
    {
        /* Check frame address */
        if (((void*)frame < NtCurrentTeb()->stack_low) ||
            ((void*)(frame+1) > NtCurrentTeb()->stack_top) ||
            (int)frame & 3)
        {
            rec->ExceptionFlags |= EH_STACK_INVALID;
            break;
        }

        /* Call handler */
        res = EXC_CallHandler( frame->Handler, EXC_RaiseHandler, rec, frame, context, &dispatch );
        if (frame == nested_frame)
        {
            /* no longer nested */
            nested_frame = NULL;
            rec->ExceptionFlags &= ~EH_NESTED_CALL;
        }

        switch(res)
        {
        case ExceptionContinueExecution:
            if (!(rec->ExceptionFlags & EH_NONCONTINUABLE)) return;
            newrec.ExceptionCode    = STATUS_NONCONTINUABLE_EXCEPTION;
            newrec.ExceptionFlags   = EH_NONCONTINUABLE;
            newrec.ExceptionRecord  = rec;
            newrec.NumberParameters = 0;
            RtlRaiseException( &newrec );  /* never returns */
            break;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            if (nested_frame < dispatch) nested_frame = dispatch;
            rec->ExceptionFlags |= EH_NESTED_CALL;
            break;
        default:
            newrec.ExceptionCode    = STATUS_INVALID_DISPOSITION;
            newrec.ExceptionFlags   = EH_NONCONTINUABLE;
            newrec.ExceptionRecord  = rec;
            newrec.NumberParameters = 0;
            RtlRaiseException( &newrec );  /* never returns */
            break;
        }
        frame = frame->Prev;
    }
    EXC_DefaultHandling( rec, context );
}


/*******************************************************************
 *         EXC_RtlUnwind
 *
 * Implementation of RtlUnwind.
 */
static void EXC_RtlUnwind( EXCEPTION_FRAME *pEndFrame, EXCEPTION_RECORD *pRecord,
                           CONTEXT *context )
{
    EXCEPTION_RECORD record, newrec;
    PEXCEPTION_FRAME frame, dispatch;

    /* build an exception record, if we do not have one */
    if (!pRecord)
    {
        record.ExceptionCode    = STATUS_UNWIND;
        record.ExceptionFlags   = 0;
        record.ExceptionRecord  = NULL;
        record.ExceptionAddress = (LPVOID)EIP_reg(context); 
        record.NumberParameters = 0;
        pRecord = &record;
    }

    pRecord->ExceptionFlags |= EH_UNWINDING | (pEndFrame ? 0 : EH_EXIT_UNWIND);
  
    /* get chain of exception frames */
    frame = NtCurrentTeb()->except;
    while ((frame != (PEXCEPTION_FRAME)0xffffffff) && (frame != pEndFrame))
    {
        /* Check frame address */
        if (pEndFrame && (frame > pEndFrame))
        {
            newrec.ExceptionCode    = STATUS_INVALID_UNWIND_TARGET;
            newrec.ExceptionFlags   = EH_NONCONTINUABLE;
            newrec.ExceptionRecord  = pRecord;
            newrec.NumberParameters = 0;
            RtlRaiseException( &newrec );  /* never returns */
        }
        if (((void*)frame < NtCurrentTeb()->stack_low) ||
            ((void*)(frame+1) > NtCurrentTeb()->stack_top) ||
            (int)frame & 3)
        {
            newrec.ExceptionCode    = STATUS_BAD_STACK;
            newrec.ExceptionFlags   = EH_NONCONTINUABLE;
            newrec.ExceptionRecord  = pRecord;
            newrec.NumberParameters = 0;
            RtlRaiseException( &newrec );  /* never returns */
        }

        /* Call handler */
        switch(EXC_CallHandler( frame->Handler, EXC_UnwindHandler, pRecord,
                                frame, context, &dispatch ))
        {
        case ExceptionContinueSearch:
            break;
        case ExceptionCollidedUnwind:
            frame = dispatch;
            break;
        default:
            newrec.ExceptionCode    = STATUS_INVALID_DISPOSITION;
            newrec.ExceptionFlags   = EH_NONCONTINUABLE;
            newrec.ExceptionRecord  = pRecord;
            newrec.NumberParameters = 0;
            RtlRaiseException( &newrec );  /* never returns */
            break;
        }
        NtCurrentTeb()->except = frame = frame->Prev;
    }
}


/*******************************************************************
 *         NtRaiseException    (NTDLL.175)
 *
 * Real prototype:
 *    DWORD WINAPI NtRaiseException( EXCEPTION_RECORD *rec, CONTEXT *ctx, BOOL first );
 */
REGS_ENTRYPOINT(NtRaiseException)
{
    DWORD ret;
    EXCEPTION_RECORD *rec;
    CONTEXT *ctx;
    BOOL first;

    ret   = STACK32_POP(context);  /* return addr */
    rec   = (PEXCEPTION_RECORD)STACK32_POP(context);
    ctx   = (PCONTEXT)STACK32_POP(context);
    first = (BOOL)STACK32_POP(context);
    STACK32_PUSH(context,ret);  /* restore return addr */

    EXC_RaiseException( rec, context );
    *context = *ctx;
}


/***********************************************************************
 *            RtlRaiseException  (NTDLL.464)
 *
 * Real prototype:
 *     void WINAPI RtlRaiseException(PEXCEPTION_RECORD pRecord)
 */
REGS_ENTRYPOINT(RtlRaiseException)
{
    EXCEPTION_RECORD *rec;
    DWORD ret;

    ret = STACK32_POP(context);  /* return addr */
    rec = (PEXCEPTION_RECORD)STACK32_POP(context);
    STACK32_PUSH(context,ret);  /* restore return addr */

    rec->ExceptionAddress = (LPVOID)EIP_reg(context);
    EXC_RaiseException( rec, context );
}


/*******************************************************************
 *         RtlUnwind  (KERNEL32.590) (NTDLL.518)
 *
 *  This function is undocumented. This is the general idea of 
 *  RtlUnwind, though. Note that error handling is not yet implemented.
 *
 * The real prototype is:
 * void WINAPI RtlUnwind( PEXCEPTION_FRAME pEndFrame, LPVOID unusedEip, 
 *                        PEXCEPTION_RECORD pRecord, DWORD returnEax );
 */
REGS_ENTRYPOINT(RtlUnwind)
{
    PEXCEPTION_FRAME pEndFrame;
    PEXCEPTION_RECORD pRecord;

    /* get the arguments from the stack */
    DWORD ret        = STACK32_POP(context);  /* return addr */
    pEndFrame        = (PEXCEPTION_FRAME)STACK32_POP(context);
    (void)STACK32_POP(context);  /* unused arg */
    pRecord          = (PEXCEPTION_RECORD)STACK32_POP(context);
    EAX_reg(context) = STACK32_POP(context);
    STACK32_PUSH(context,ret);  /* restore return addr */

    EXC_RtlUnwind( pEndFrame, pRecord, context );
}


/***********************************************************************
 *            RtlRaiseStatus  (NTDLL.465)
 *
 * Raise an exception with ExceptionCode = status
 */
void WINAPI RtlRaiseStatus( NTSTATUS status )
{
    EXCEPTION_RECORD ExceptionRec;

    ExceptionRec.ExceptionCode    = status;
    ExceptionRec.ExceptionFlags   = EH_NONCONTINUABLE;
    ExceptionRec.ExceptionRecord  = NULL;
    ExceptionRec.NumberParameters = 0;
    RtlRaiseException( &ExceptionRec );
}
