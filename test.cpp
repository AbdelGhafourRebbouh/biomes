#include <windows.h>
#include <iostream>
using namespace std;
int main() {
    // 1. Test standard C++ output
    cout << "[Biomes Setup Test] C++ Compiler is working!" << endl;

    // 2. Test Windows API call: Fetch current active window handle
    HWND activeWindow = GetForegroundWindow();

    if (activeWindow != NULL) {
        // Get the length of the window title
        int titleLength = GetWindowTextLengthA(activeWindow);
        
        if (titleLength > 0) {
            char title[256];
            GetWindowTextA(activeWindow, title, sizeof(title));
            cout << "[Biomes Setup Test] Active Window Title: " << title << endl;
        }
    }

    cout << "[SUCCESS] Your environment is 100% ready for Biomes development!" << endl;
    return 0;
}