#define X64_GPR(_) \
    _(RAX), \
    _(RCX), \
    _(RDX), \
    _(RBX), \
    _(RSP), \
    _(RBP), \
    _(RSI), \
    _(RDI), \
    _(R8), \
    _(R9), \
    _(R10), \
    _(R11), \
    _(R12), \
    _(R13), \
    _(R14), \
    _(R15)


#define X64_SSE(_) \
    _(XMM0), \
    _(XMM1), \
    _(XMM2), \
    _(XMM3), \
    _(XMM4), \
    _(XMM5), \
    _(XMM6), \
    _(XMM7)

/* declare enums */

enum {
X64_GPR(MVM_JIT_REG)
};

enum {
X64_SSE(MVM_JIT_REG)
};


#if MVM_JIT_PLATFORM == MVM_JIT_PLATFORM_POSIX
/* Define the GPR set usable for general calculations */

#define X64_FREE_GPR(_) \
    _(RAX), \
    _(RCX), \
    _(RDX), \
    _(RSI), \
    _(RDI), \
    _(R8), \
    _(R9), \
    _(R10), \
    _(R11)

/* define set of non-volatile regsiters */


#define X64_NVR(_) \
    _(RBX), \
    _(RSP), \
    _(RBP), \
    _(R12), \
    _(R13), \
    _(R14), \
    _(R15)

/* GPR used for arguments */
#define X64_ARG_GPR(_) \
    _(RDI), \
    _(RSI), \
    _(RDX), \
    _(RCX), \
    _(R8), \
    _(R9)

/* SSE used for arguments */

#define X64_ARG_SSE(_) \
    X64_SSE(_)

#else

/* Microsoft why you give us so few registers :-( */
#define X64_FREE_GPR(_) \
    _(RAX), \
    _(RCX), \
    _(RDX), \
    _(R8), \
    _(R9), \
    _(R10), \
    _(R11)
#define X64_NVR(_) \
    _(RBX), \
    _(RSP), \
    _(RBP), \
    _(RSI), \
    _(RDI), \
    _(R12), \
    _(R13), \
    _(R14), \
    _(R15)
#define X64_ARG_GPR(_) \
    _(RCX), \
    _(RDX), \
    _(R8), \
    _(R9)
#define X64_ARG_SSE(_) \
    _(XMM0), \
    _(XMM1), \
    _(XMM2), \
    _(XMM3)
#endif

/* Frame declarations */
#define MVM_JIT_MAX_GPR 16