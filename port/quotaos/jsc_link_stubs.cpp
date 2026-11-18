/* Symbols the CLoop image references from JIT, bytecode cache, and
 * exception paths. Calling one stops the guest. It does not return a result. */
extern "C" int printf(const char *fmt, ...);
extern "C" void abort(void);

extern "C" double frexp(double x, int *exp)
{
    return __builtin_frexp(x, exp);
}

extern "C" double ldexp(double x, int exp)
{
    return __builtin_ldexp(x, exp);
}

extern "C" double __frexp(double x, int *exp)
{
    return frexp(x, exp);
}

extern "C" double __ldexp(double x, int exp)
{
    return ldexp(x, exp);
}

static void stop(const char *name)
{
    printf("[jsc] unimplemented %s\n", name);
    abort();
}

extern "C" void _ZSt28__throw_bad_array_new_lengthv(void) { stop("bad_array_new_length"); }
extern "C" void _ZSt17__throw_bad_allocv(void) { stop("bad_alloc"); }
extern "C" void _ZSt20__throw_system_errori(int) { stop("system_error"); }
extern "C" void _ZSt20__throw_length_errorPKc(const char *) { stop("length_error"); }
extern "C" void _ZSt19__throw_logic_errorPKc(const char *) { stop("logic_error"); }
extern "C" void _ZSt25__throw_bad_function_callv(void) { stop("bad_function_call"); }
extern "C" void _ZSt9terminatev(void) { stop("terminate"); }

extern "C" void _ZNSt18condition_variableC1Ev(void) {}
extern "C" void _ZNSt18condition_variableD1Ev(void) {}
extern "C" void _ZNSt18condition_variable10notify_oneEv(void) {}
extern "C" void _ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE(void *, void *)
{
    stop("condition_variable::wait");
}

extern "C" void _ZN3WTF14FileSystemImpl10FileHandle3mapENS0_14MappedFileModeENS0_12FileOpenModeE(void)
{
    stop("FileHandle::map");
}

extern "C" void _ZNK3JSC12CachePayload4spanEv(void) { stop("CachePayload::span"); }
extern "C" void _ZN3JSC12CachePayloadC1EOS0_(void *) {}
extern "C" void _ZN3JSC12CachePayloadD1Ev(void *) {}
extern "C" void _ZN3JSC12CachePayload17makeMappedPayloadEON3WTF14FileSystemImpl14MappedFileDataE(void)
{
    stop("CachePayload::makeMappedPayload");
}
extern "C" void _ZN3JSC12CachePayload17makeMallocPayloadEON3WTF10MallocSpanIhNS1_10FastMallocEEE(void)
{
    stop("CachePayload::makeMallocPayload");
}

extern "C" void _ZN3JSC13genericUnwindERNS_2VMEPNS_9CallFrameE(void) { stop("genericUnwind"); }
extern "C" void _ZN3JSC23AssemblyCommentRegistry10initializeEv(void) {}
extern "C" void _ZN3JSC4Wasm10makeStringERKNS0_11IndexOrNameE(void) { stop("Wasm::makeString"); }
extern "C" int _ZN3JSC24shouldDumpDisassemblyForEPNS_9CodeBlockE(void) { return 0; }
extern "C" void _ZNK3JSC26PolymorphicCallStubRoutine8variantsEv(void) { stop("PolymorphicCallStubRoutine::variants"); }
extern "C" void _ZN3JSC26PolymorphicCallStubRoutineC1EjjRKNS_21MacroAssemblerCodeRefILN3WTF6PtrTagE49594EEERNS_2VMEPNS_6JSCellEPNS_9CallFrameERNS_12CallLinkInfoERKNS2_6VectorINS_8CallSlotELm16ENS2_15CrashOnOverflowELm16ENS2_10FastMallocEEEbb(void)
{
    stop("PolymorphicCallStubRoutine");
}
extern "C" void _ZN3JSC14JITStubRoutine19observeZeroRefCountEv(void) {}
extern "C" void _ZN3JSC19PolymorphicCallNode19unlinkOrUpgradeImplERNS_2VMEPNS_9CodeBlockES4_(void) { stop("PolymorphicCallNode"); }
extern "C" void _ZN3JSC26PolymorphicCallStubRoutine16unlinkForcefullyEv(void) {}
extern "C" int _ZN3JSC14JITStubRoutine9visitWeakERNS_2VME(void) { return 0; }
extern "C" void _ZN3JSC12dumpBytecodeEPNS_18BytecodeDumperBaseINS_17InstructionStreamINS_15BaseInstructionINS_14JSOpcodeTraitsEEEEEEEjPKS4_(void) {}
