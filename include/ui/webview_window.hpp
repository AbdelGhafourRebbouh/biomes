#pragma once
#ifndef WEBVIEW_WINDOW_HPP
#define WEBVIEW_WINDOW_HPP

#include <windows.h>
#include <string>
#include <functional>
#include <WebView2.h>

class WebViewWindow {
public:
    // Initialize Win32 container window & WebView2 environment
    static bool Initialize(HINSTANCE hInstance, int nCmdShow, const std::string& startUrl);
    
    // Process Windows message loop
    static void RunMessageLoop();

    // Send JSON payload downstream from C++ to JavaScript
    static void SendMessageToUI(const std::string& jsonPayload);

    // Register callback function to receive upstream JSON commands from JavaScript
    static void SetMessageReceivedCallback(std::function<void(const std::string&)> callback);

private:
    static HWND s_hwnd;
    static ICoreWebView2Controller* s_controller;
    static ICoreWebView2* s_webview;
    static std::function<void(const std::string&)> s_onMessageReceived;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void InitWebView(const std::string& startUrl);
};

#endif // WEBVIEW_WINDOW_HPP