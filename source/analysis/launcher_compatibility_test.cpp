#include "../launcher.cpp"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    using namespace CascadeurQtCompatibility;
    static_assert(supported(6,0,0) && supported(6,5,3) && supported(6,99,99));
    static_assert(!supported(5,15,2) && !supported(7,0,0));
    if (!fileVersion(argv[1]) || !fileVersion(argv[2]) || !IsAmd64Image(argv[1]) ||
        fileVersion(L"missing.dll") || IsAmd64Image(L"missing.exe")) return 2;
    if (!supportedDirectory(ParentDir(argv[1]))) return 4;
    if (!supportedRuntime("6.0.0") || !supportedRuntime("6.5.3") ||
        !supportedRuntime("6.99.10") || supportedRuntime("5.15.2") ||
        supportedRuntime("7.0.0") || supportedRuntime(nullptr)) return 5;
    if (ParentDir(L"C:\\folder\\file.exe") != L"C:\\folder") return 3;
    std::puts("PASS: coherent Qt 6 capability family accepted; missing images rejected");
    return 0;
}
