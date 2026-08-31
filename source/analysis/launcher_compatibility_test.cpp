#include "../launcher.cpp"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    using namespace CascadeurQtCompatibility;
    static_assert(supported(6,5,1) && supported(6,5,3));
    static_assert(!supported(6,5,0) && !supported(6,5,2) && !supported(6,6,0) && !supported(5,15,2));
    if (!fileVersion(argv[1]) || !IsAmd64Image(argv[1]) ||
        fileVersion(argv[2]) || fileVersion(L"missing.dll") || IsAmd64Image(L"missing.exe")) return 2;
    if (!supportedDirectory(ParentDir(argv[1]))) return 4;
    if (!supportedRuntime("6.5.1") || !supportedRuntime("6.5.3") || supportedRuntime("6.5.2") ||
        supportedRuntime("6.5.10") || supportedRuntime(nullptr)) return 5;
    if (ParentDir(L"C:\\folder\\file.exe") != L"C:\\folder") return 3;
    std::puts("PASS: supported Qt x64 directory accepted; unsupported Qt and missing images rejected");
    return 0;
}
