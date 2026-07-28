#include "../include/core/window_scaler.hpp"
#include <iostream>
using namespace std;

int main() {
    cout << "--- TESTING PHASE 1: WINDOW SCALER ---" << endl;

    // 1. Fetch active windows
    vector<WindowInfo> activeApps = WindowScaler::GetActiveWindows();

    cout << "Found " << activeApps.size() << " active windows on your desktop:\n" << endl;

    for (size_t i = 0; i < activeApps.size(); ++i) {
        cout << "[" << i + 1 << "] Title: " << activeApps[i].title << endl;
        cout << "    Position: Left=" << activeApps[i].rect.left 
                  << ", Top=" << activeApps[i].rect.top 
                  << ", Width=" << (activeApps[i].rect.right - activeApps[i].rect.left)
                  << ", Height=" << (activeApps[i].rect.bottom - activeApps[i].rect.top) << "\n" << endl;
    }

    return 0;
}