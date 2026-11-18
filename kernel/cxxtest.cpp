/* Guest check that the freestanding C++ runtime can construct an object. */
struct CxxProbe {
    int value;
    CxxProbe() : value(41) {}
    virtual int bump() { return value + 1; }
    virtual ~CxxProbe() {}
};

extern "C" int cxx_runtime_test(void)
{
    CxxProbe *p = new CxxProbe;
    if (!p)
        return -1;
    int n = p->bump();
    delete p;
    void *a = operator new(32);
    void *b = operator new(32);
    operator delete(a);
    void *c = operator new(32);
    if (c != a || c == b)
        return -2;
    operator delete(b);
    operator delete(c);
    return n;
}
