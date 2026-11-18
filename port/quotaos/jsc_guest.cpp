#include <JavaScriptCore/JavaScript.h>

extern "C" {
int quotaos_webcore_paint_hex(const char *hex, unsigned hex_len,
    unsigned *rgba, unsigned *painted, unsigned *total);
void jsc_arena_init(void);
void jsc_enable_fp_and_tls(void);
void jsc_run_constructors(void);
unsigned long jsc_icu_call_count(void);
int printf(const char *fmt, ...);
void abort(void);

void __once_proxy(void);
void _ZN9Inspector20JSInjectedScriptHost7destroyEPN3JSC6JSCellE(void *);
void _ZN9Inspector21JSJavaScriptCallFrame7destroyEPN3JSC6JSCellE(void *);
void _ZN9Inspector15ScriptArgumentsD1Ev(void *);
void *_ZN9Inspector21createScriptArgumentsEPN3JSC14JSGlobalObjectEPNS0_9CallFrameEj(void *, void *, unsigned);
void *_ZN9Inspector15ScriptArguments6createEPN3JSC14JSGlobalObjectEON3WTF6VectorINS1_6StrongINS1_7UnknownELNS1_30ShouldStrongDestructorGrabLockE0EEELm0ENS4_15CrashOnOverflowELm16ENS4_10FastMallocEEE(void *, void *);
}

namespace std {
thread_local void (*__once_call)();
thread_local void *__once_callable;
}

extern "C" void __once_proxy(void)
{
    if (std::__once_call)
        std::__once_call();
}

extern "C" void _ZN9Inspector20JSInjectedScriptHost7destroyEPN3JSC6JSCellE(void *) {}
extern "C" void _ZN9Inspector21JSJavaScriptCallFrame7destroyEPN3JSC6JSCellE(void *) {}
extern "C" void _ZN9Inspector15ScriptArgumentsD1Ev(void *) {}
extern "C" void *_ZN9Inspector21createScriptArgumentsEPN3JSC14JSGlobalObjectEPNS0_9CallFrameEj(void *, void *, unsigned)
{
    return 0;
}
extern "C" void *_ZN9Inspector15ScriptArguments6createEPN3JSC14JSGlobalObjectEON3WTF6VectorINS1_6StrongINS1_7UnknownELNS1_30ShouldStrongDestructorGrabLockE0EEELm0ENS4_15CrashOnOverflowELm16ENS4_10FastMallocEEE(void *, void *)
{
    return 0;
}

static int near(double v, double want)
{
    double d = v - want;
    if (d < 0)
        d = -d;
    return d < 0.0001;
}

extern "C" int quotaos_jsc_guest(void)
{
    jsc_arena_init();
    jsc_enable_fp_and_tls();
    jsc_run_constructors();

    JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
    if (!ctx) {
        printf("[jsc] JSGlobalContextCreate returned null\n");
        return 2;
    }
    JSValueRef ex = nullptr;
    JSStringRef code = JSStringCreateWithUTF8CString("1+2");
    JSValueRef value = JSEvaluateScript(ctx, code, nullptr, nullptr, 1, &ex);
    double number = 0;
    int number_ok = 0;
    if (!ex && value) {
        JSValueRef ex2 = nullptr;
        number = JSValueToNumber(ctx, value, &ex2);
        number_ok = !ex2;
    }
    printf("[jsc] 1+2 -> %s %lu/1000 icu %lu\n", number_ok ? "number" : "fail",
           (unsigned long)(number < 0 ? 0 : number * 1000.0), jsc_icu_call_count());
    JSStringRelease(code);

    JSValueRef exs = nullptr;
    JSStringRef text = JSStringCreateWithUTF8CString("'ab'+'c'");
    JSValueRef svalue = JSEvaluateScript(ctx, text, nullptr, nullptr, 1, &exs);
    char buf[32];
    buf[0] = 0;
    int string_ok = 0;
    if (!exs && svalue) {
        JSValueRef ex3 = nullptr;
        JSStringRef out = JSValueToStringCopy(ctx, svalue, &ex3);
        if (!ex3 && out) {
            JSStringGetUTF8CString(out, buf, (int)sizeof buf);
            string_ok = 1;
            JSStringRelease(out);
        }
    }
    printf("[jsc] 'ab'+'c' -> %s [%s] icu %lu\n", string_ok ? "string" : "fail", buf,
           jsc_icu_call_count());
    JSStringRelease(text);
    JSGlobalContextRelease(ctx);

    int pass = number_ok && near(number, 3.0) && string_ok && buf[0] == 'a' && buf[1] == 'b' &&
               buf[2] == 'c' && buf[3] == 0 && jsc_icu_call_count() == 0;
    printf("[jsc] guest %s\n", pass ? "PASS" : "FAIL");

    unsigned rgba = 0, painted = 0, total = 0;
    int painted_ok = quotaos_webcore_paint_hex("c80000", 6, &rgba, &painted, &total);
    unsigned ignored = 0, ignored_n = 0, ignored_t = 0;
    int rejected = quotaos_webcore_paint_hex("zz", 2, &ignored, &ignored_n, &ignored_t);
    printf("[webcore] CSSParserFastPaths hex c80000 rgba 0x%x painted %u/%u ok %d reject_zz %d\n",
           rgba, painted, total, painted_ok, rejected);
    return pass ? 0 : 1;
}
