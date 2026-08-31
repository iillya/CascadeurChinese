// Harmless PE fixture: never loads Qt or injects a process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int main() { Sleep(60000); return 0; }
