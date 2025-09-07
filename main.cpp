#include <windows.h> // Required for Windows API functions
#include <string>    // For std::string and std::to_string
#include <iomanip>   // For std::fixed and std::setprecision
#include <sstream>   // For std::ostringstream
#include "pugixml.hpp"  //requiered library for XML parsing
#include <memory>
#include <stdexcept>
#include <iostream>
#include <cstdio>
#include <array>
#include <vector>
#include <shlwapi.h> // For PathFileExistsA, to search the path of nvidia-smi.exe

// Define a unique ID for our timer
#define CPU_USAGE_TIMER_ID 1 
#define WINDOW_H 400 // Horizontal of the window
#define WINDOW_V 200 // Vertical of the window

/**
 * Program structure:
 * CPU BLOCK
 *      getCurrentCpuUsage() - Calculates the current CPU usage percentage.
 * RAM BLOCK
 *      getCurrentRamUsage() - Calculates the current RAM usage in gigabytes.
 * GPU BLOCK
 *      getNVSMIPath() - Determines the best path to nvidia-smi.exe.
 *          GetNVSMIPathFromRegistry() - Tries to get the path from the registry.
 *          findInPath() - Searches for nvidia-smi.exe in the system PATH.
 *          fileExists() - Checks if a file exists at a given path.
 *      exec_no_console() - Child process execution function to run nvidia-smi.exe and capture its output.
 *      getXmlGpuData() - Obtains the GPU data
 *      parseGpuData() - Parses the XML output from nvidia-smi.exe to extract GPU information.
 * WINDOW AND RENDERING BLOCK
 *      refreshAllData() - Refreshes CPU, RAM, and GPU data and updates the display text.
 *      wndProc() - Window procedure function to handle messages, updates display
 *      WinMain() - Main function to create the window and start the message loop.
 */

// Global variable to store all data text
// Reminder: If you switch to Unicode (no 'A' suffix on functions), this should be wchar_t.
char g_statsText[1024] = "Initializing..."; // Buffer to hold the stats display text
bool g_gpuDataAvailable = false; // Flag to indicate if GPU data is available

// A helper function to convert FILETIME to a 64-bit integer
unsigned long long FileTimeToInt64(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Static variables to store previous CPU times for calculation
static unsigned long long previousIdleTime = 0;
static unsigned long long previousKernelTime = 0;
static unsigned long long previousUserTime = 0;
static bool firstCall = true; // Flag for the first call to initialize previous times

// A structure to hold the GPU data
struct GpuData {
    std::string name;
    std::string driverVersion;
    unsigned int temperature;
    double memoryTotal;
    
    double memoryUsed;
    unsigned int utilizationGpu;
};

GpuData g_gpuData; // Global variable to hold GPU data

// CPU BLOCK

// Function to calculate current CPU usage
double getCurrentCpuUsage() {
    FILETIME idleTime, kernelTime, userTime;

    // Get the current system times, return -1.0 on failure
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return -1.0;
    }

    // Convert FILETIME to 64-bit integers to calculate cpu times
    unsigned long long currentIdleTime = FileTimeToInt64(idleTime);
    unsigned long long currentKernelTime = FileTimeToInt64(kernelTime);
    unsigned long long currentUserTime = FileTimeToInt64(userTime);

    // If this is the very first call, initialize previous values and return 0.0
    // This prevents a large, incorrect initial spike.
    if (firstCall) {
        previousIdleTime = currentIdleTime;
        previousKernelTime = currentKernelTime;
        previousUserTime = currentUserTime;
        firstCall = false;
        return 0.0;
    }

    // delta times
    unsigned long long idleTimeDelta = currentIdleTime - previousIdleTime;
    unsigned long long kernelTimeDelta = currentKernelTime - previousKernelTime;
    unsigned long long userTimeDelta = currentUserTime - previousUserTime;

    // Total time includes both kernel and user time.
    // GetSystemTimes' kernel time *includes* idle time.
    // So, total CPU activity is (kernelTimeDelta + userTimeDelta).
    // The CPU usage is then 1.0 - (idleTimeDelta / totalActivityTime).
    unsigned long long totalActivityTime = kernelTimeDelta + userTimeDelta;

    // Update previous values for the next call
    previousIdleTime = currentIdleTime;
    previousKernelTime = currentKernelTime;
    previousUserTime = currentUserTime;

    if (totalActivityTime == 0) {
        return 0.0; // Avoid division by zero
    }

    // Calculate CPU usage percentage
    return (1.0 - (static_cast<double>(idleTimeDelta) / totalActivityTime)) * 100.0;
}

// RAM BLOCK

// This function calculates and returns the current RAM usage in gigabytes (GB)
double getCurrentRamUsage() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);

    // Calculate the used physical memory
    // total physical memory - available physical memory
    ULONGLONG totalPhysMem = memInfo.ullTotalPhys;
    ULONGLONG availPhysMem = memInfo.ullAvailPhys;
    ULONGLONG usedPhysMem = totalPhysMem - availPhysMem;

    // Convert bytes to gigabytes (GB)
    return static_cast<double>(usedPhysMem) / (1024.0 * 1024.0 * 1024.0);
}

// GPU BLOCK

// Function to create a child process and capture its output without showing a console window
std::string exec_no_console(const char* cmd) {
    HANDLE hReadPipe, hWritePipe;
    
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        throw std::runtime_error("CreatePipe failed!");
    }
    
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Create a modifiable copy of the command string
    std::string command_str(cmd);
    std::vector<char> cmd_copy(command_str.begin(), command_str.end());
    cmd_copy.push_back('\0');

    // Create the child process. The first parameter is NULL, so CreateProcessA
    // will parse the full command line string from cmd_copy.data().
    if (!CreateProcessA(NULL,             // Use command line for application name
                        cmd_copy.data(),  // Command line
                        NULL,             // Process handle not inheritable
                        NULL,             // Thread handle not inheritable
                        TRUE,             // Set handle inheritance to TRUE
                        CREATE_NO_WINDOW, // Prevents a console window
                        NULL,             // Use parent's environment block
                        NULL,             // Use parent's starting directory 
                        &si,              // Pointer to STARTUPINFO structure
                        &pi)              // Pointer to PROCESS_INFORMATION structure
    ) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        // Get the specific error code to diagnose further
        DWORD errorCode = GetLastError();
        std::ostringstream error_oss;
        error_oss << "CreateProcess failed with error code: " << errorCode;
        throw std::runtime_error(error_oss.str());
    }
    
    CloseHandle(hWritePipe);
    std::string result;
    DWORD dwRead;
    char buffer[32768];
    while (ReadFile(hReadPipe, buffer, sizeof(buffer), &dwRead, NULL) && dwRead > 0) {
        result.append(buffer, dwRead);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);
    return result;
}

// Helper: Check if a file exists
bool fileExists(const std::string& path) {
    return PathFileExistsA(path.c_str());
}

// Helper: Try to get NVSMI path from registry
std::string getNVSMIPathFromRegistry() {
    HKEY hKey;
    char value[512];
    DWORD value_length = sizeof(value);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NVSMI", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "Path", NULL, NULL, (LPBYTE)value, &value_length) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            std::string path = value;
            if (!path.empty() && path.back() != '\\') path += '\\';
            path += "nvidia-smi.exe";
            if (fileExists(path)) return path;
        }
        RegCloseKey(hKey);
    }
    return "";
}

// Helper: Search for nvidia-smi.exe in PATH
std::string findInPath(const std::string& exeName) {
    char* envPath = nullptr;
    size_t len = 0;
    _dupenv_s(&envPath, &len, "PATH");
    if (!envPath) return "";
    std::string result;
    std::istringstream iss(envPath);
    std::string dir;
    while (std::getline(iss, dir, ';')) {
        if (!dir.empty() && dir.back() != '\\') dir += '\\';
        std::string fullPath = dir + exeName;
        if (fileExists(fullPath)) {
            result = fullPath;
            break;
        }
    }
    free(envPath);
    return result;
}

// Main function to get the best path to nvidia-smi.exe
std::string getNVSMIPath() {
    // 1. Try registry
    std::string path = getNVSMIPathFromRegistry();
    if (!path.empty()) return path;

    // 2. Try common install locations
    std::vector<std::string> commonPaths = {
        "C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe",
        "C:\\Windows\\System32\\nvidia-smi.exe",
        "C:\\Windows\\Sysnative\\nvidia-smi.exe"
    };
    for (const auto& p : commonPaths) {
        if (fileExists(p)) return p;
    }

    // 3. Try same folder as this executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = exePath;
    size_t pos = exeDir.find_last_of("\\/");
    if (pos != std::string::npos) {
        exeDir = exeDir.substr(0, pos + 1) + "nvidia-smi.exe";
        if (fileExists(exeDir)) return exeDir;
    }

    // 4. Try PATH
    path = findInPath("nvidia-smi.exe");
    if (!path.empty()) return path;

    // 5. Fallback: just the name (let CreateProcess try PATH)
    return "nvidia-smi.exe";
}

// Function to get the XML output from nvidia-smi, adapted to use the full path from the registry
std::string getXmlGpuData() {
    std::string nvsmiExePath = getNVSMIPath();
    std::string command;
    // Only quote if it's a full path (contains \ or space)
    if (nvsmiExePath.find('\\') != std::string::npos || nvsmiExePath.find(' ') != std::string::npos) {
        command = "\"" + nvsmiExePath + "\" -q -x";
    } else {
        command = nvsmiExePath + " -q -x";
    }
    return exec_no_console(command.c_str());
}

// Function to parse the GPU data from the XML string
GpuData parseGpuData(const pugi::xml_document& doc) {
    GpuData data;
    pugi::xml_node gpu_node = doc.child("nvidia_smi_log").child("gpu");

    if (gpu_node) {
        data.name = std::string(gpu_node.child("product_name").text().get());
        data.driverVersion = doc.child("nvidia_smi_log").child("driver_version").text().get();
        
        // Temperature
        pugi::xml_node temperature_node = gpu_node.child("temperature").child("gpu_temp");
        if (temperature_node) {
            data.temperature = std::stoul(temperature_node.text().get());
        }

        // Memory Usage
        pugi::xml_node memory_node = gpu_node.child("fb_memory_usage");
        if (memory_node) {
            data.memoryTotal = std::stoul(memory_node.child("total").text().get());
            data.memoryUsed = std::stoul(memory_node.child("used").text().get());
            data.memoryTotal /= 1024; // Convert from MB to GB
            data.memoryUsed /= 1024;  // Convert from MB to GB
        }

        // Utilization
        pugi::xml_node utilization_node = gpu_node.child("utilization");
        if (utilization_node) {
            data.utilizationGpu = std::stoul(utilization_node.child("gpu_util").text().get());
        }
    }
    return data;
}

// WINDOW AND RENDERING BLOCK

// Function to refresh all data (CPU, RAM, and GPU)
void refreshAllData(HWND hwnd) {
    double cpuUsage = getCurrentCpuUsage();
    double ramUsage = getCurrentRamUsage();

    // GPU data retrieval
    // Wrap GPU data retrieval in try-catch to handle potential errors
    try {
        std::string xmlOutput = getXmlGpuData();
        if (!xmlOutput.empty()) {
            pugi::xml_document doc;
            pugi::xml_parse_result result = doc.load_string(xmlOutput.c_str());
            if (result) {
                g_gpuData = parseGpuData(doc);
                g_gpuDataAvailable = true;
            } else {
                g_gpuDataAvailable = false;
            }
        } else {
            g_gpuDataAvailable = false;
        }
    } catch (const std::exception& e) {
        g_gpuDataAvailable = false;
        std::cerr << "Error getting GPU data: " << e.what() << std::endl;
    }

    // Format the stats text
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    
    if (cpuUsage >= 0) {
        oss << "CPU Usage: " << cpuUsage << "%\n"
            << "RAM Usage: " << ramUsage << " GB\n";
    } else {
        oss << "Error getting CPU/RAM usage.\n";
    }

    if (g_gpuDataAvailable) {
        oss << "\n--- GPU Stats ---\n"
            << "GPU: " << g_gpuData.name << "\n"
            << "Temp: " << g_gpuData.temperature << " C\n"
            << "VRAM: " << g_gpuData.memoryUsed << " GB / " << g_gpuData.memoryTotal << " GB\n"
            << "GPU Util: " << g_gpuData.utilizationGpu << " %";
    } else {
        oss << "\n--- GPU Stats ---\n"
            << "GPU data not available or initializing...";
    }
    strncpy_s(g_statsText, sizeof(g_statsText), oss.str().c_str(), _TRUNCATE);
    // Optionally, update g_cpuUsageText for legacy code compatibility
    InvalidateRect(hwnd, NULL, TRUE);
}

// Window Procedure function - handles messages sent to the window
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Set a timer to update CPU usage every 250 milliseconds (4 times per second)
            SetTimer(hwnd, CPU_USAGE_TIMER_ID, 250, NULL);
            // Perform an initial call to getCurrentCpuUsage to set up static variables
            // for correct delta calculations on subsequent timer ticks.
            getCurrentCpuUsage();
            break;
        }
        case WM_ENTERSIZEMOVE: {
            // Handle window resizing if needed
            KillTimer(hwnd, CPU_USAGE_TIMER_ID); // Stop the timer while resizing
            break;
        }
        case WM_EXITSIZEMOVE: {
            // Restart the timer after resizing
            SetTimer(hwnd, CPU_USAGE_TIMER_ID, 250, NULL);
            refreshAllData(hwnd); // Refresh data after resizing
            break;
        }
        case WM_TIMER: {
            if (wParam == CPU_USAGE_TIMER_ID) {
                refreshAllData(hwnd);
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps); // Get a device context for painting

            // Get the client area rectangle
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            // Set text color (e.g., black)
            SetTextColor(hdc, RGB(0, 0, 0));
            // Set background mode to transparent so text doesn't draw a solid background rectangle
            SetBkMode(hdc, TRANSPARENT);

            // 1. Calculate the text rectangle size
            RECT textRect = {0, 0, clientRect.right - clientRect.left, 0};
            DrawTextA(hdc, g_statsText, -1, &textRect, DT_WORDBREAK | DT_CALCRECT);
            int textWidth = textRect.right;
            int textHeight = textRect.bottom;

            // 2. Calculate the correct top and left positions for centering
            int top = (clientRect.bottom - textHeight) / 2;
            int left = (clientRect.right - textWidth) / 2;

            // 3. Update the textRect with the new, centered coordinates
            textRect.left = left;
            textRect.top = top;
            textRect.right = left + textWidth;
            textRect.bottom = top + textHeight;

            // Draw the CPU usage text in the center of the window
            // DT_SINGLELINE | DT_CENTER | DT_VCENTER will center the text
            DrawTextA(hdc, g_statsText, -1, &textRect, DT_WORDBREAK);

            EndPaint(hwnd, &ps); // Release the device context
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY:
            KillTimer(hwnd, CPU_USAGE_TIMER_ID); // Clean up the timer
            PostQuitMessage(0); // Post a message to terminate the application
            break;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam); // Default message processing (ANSI version)
    }
    return 0;
}

// WinMain is the entry point for Windows GUI applications
int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    WNDCLASSEXA wc = {0}; // Using WNDCLASSEXA for ANSI compatibility

    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorA(NULL, MAKEINTRESOURCEA(IDC_ARROW)); // Corrected LoadCursorA usage
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // Default window background color
    wc.lpszClassName = "CpuMonitorClass";

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Window Registration Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        "CpuMonitorClass",
        "Stats display", // Initial window title
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        WINDOW_H, WINDOW_V, // Adjusted window size for better text display
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (hwnd == NULL) {
        MessageBoxA(NULL, "Window Creation Failed!", "Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    HANDLE hProcess = GetCurrentProcess();

    // Set the process priority to IDLE_PRIORITY_CLASS, lower than NORMAL_PRIORITY_CLASS,
    // in order to minimize CPU usage of this monitoring application when system is under load.
    if (!SetPriorityClass(hProcess, IDLE_PRIORITY_CLASS)) {
        MessageBoxA(NULL, "Failed to set process priority to low (IDLE_PRIORITY_CLASS)",
             "Error", MB_OK | MB_ICONERROR);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg
        , NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return static_cast<int>(msg.wParam);
}
// comand line to compile:
// cl main.cpp pugixml.cpp user32.lib gdi32.lib kernel32.lib Advapi32.lib Shlwapi.lib /EHsc /Festats_display.exe
// /Festats_display.exe to name the output file, same as -o in gcc.
// /EHsc to enable C++ exceptions.