#include <windows.h>
#include <iostream>

int main() {
    // 1. Test standard C++ output
    std::cout << "[Biomes Setup Test] C++ Compiler is working!" << std::endl;

    // 2. Test Windows API call: Fetch current active window handle
    HWND activeWindow = GetForegroundWindow();

    if (activeWindow != NULL) {
        // Get the length of the window title
        int titleLength = GetWindowTextLengthA(activeWindow);
        
        if (titleLength > 0) {
            char title[256];
            GetWindowTextA(activeWindow, title, sizeof(title));
            std::cout << "[Biomes Setup Test] Active Window Title: " << title << std::endl;
        }
    }

    std::cout << "[SUCCESS] Your environment is 100% ready for Biomes development!" << std::endl;
    return 0;
}