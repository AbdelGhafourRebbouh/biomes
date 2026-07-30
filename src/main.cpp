#include "../include/core/window_scaler.hpp"
#include "../include/core/biome_manager.hpp"
#include <iostream>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "--- TESTING PHASE 2: LAYOUT ENGINE ---" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Simulate a standard 1080p monitor screen space
    int screenWidth = 1920;
    int screenHeight = 1080;
    int gap = 10;

    // ------------------------------------------------------------------
    // TEST 1: Mode A - WindowGrid Matrix (3 Rows x 4 Columns)
    // ------------------------------------------------------------------
    std::cout << "--- Mode A: WindowGrid Matrix (3x4) ---" << std::endl;
    std::vector<GridBox> grid = BiomeManager::GenerateWindowGrid(screenWidth, screenHeight, 3, 4, gap);

    std::cout << "Generated " << grid.size() << " grid boxes:\n" << std::endl;
    for (size_t i = 0; i < grid.size() && i < 4; ++i) { // Print first 4 boxes
        std::cout << "Box [" << grid[i].id << "] -> Position: (" 
                  << grid[i].x << ", " << grid[i].y << ") | Dimensions: " 
                  << grid[i].width << "x" << grid[i].height << "px" << std::endl;
    }

    std::cout << "\n----------------------------------------\n" << std::endl;

    // ------------------------------------------------------------------
    // TEST 2: Mode B - Hyprland Dynamic Binary Split
    // ------------------------------------------------------------------
    std::cout << "--- Mode B: Hyprland Binary Split ---" << std::endl;
    
    // Master Box (Full Screen minus padding)
    GridBox master;
    master.id = 0;
    master.x = gap;
    master.y = gap;
    master.width = screenWidth - (2 * gap);
    master.height = screenHeight - (2 * gap);

    std::cout << "Master Box: " << master.width << "x" << master.height << "px" << std::endl;

    // First Split: Vertical (Cut into Left and Right child boxes)
    auto [left, right] = BiomeManager::SplitBox(master, SplitDirection::VERTICAL, gap);
    std::cout << "\nAfter Vertical Split:" << std::endl;
    std::cout << "  Left Child:  (" << left.x << ", " << left.y << ") | " << left.width << "x" << left.height << "px" << std::endl;
    std::cout << "  Right Child: (" << right.x << ", " << right.y << ") | " << right.width << "x" << right.height << "px" << std::endl;

    // Second Split: Split the Right Child Horizontally (Top and Bottom)
    auto [topRight, bottomRight] = BiomeManager::SplitBox(right, SplitDirection::HORIZONTAL, gap);
    std::cout << "\nAfter Splitting Right Child Horizontally:" << std::endl;
    std::cout << "  Top Right Box:    (" << topRight.x << ", " << topRight.y << ") | " << topRight.width << "x" << topRight.height << "px" << std::endl;
    std::cout << "  Bottom Right Box: (" << bottomRight.x << ", " << bottomRight.y << ") | " << bottomRight.width << "x" << bottomRight.height << "px" << std::endl;

    return 0;
}