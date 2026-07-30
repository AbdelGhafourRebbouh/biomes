#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>

struct GridBox {
    int id;
    int x;
    int y;
    int width;
    int height;
    std::string assignedAppTitle;
};
// enum class is for scoping the enum values to avoid name clashes and improve code clarity
enum class SplitDirection { NONE, HORIZONTAL, VERTICAL };

struct TreeNode {
    GridBox box;
    SplitDirection split = SplitDirection::NONE;
    TreeNode* left = nullptr;
    TreeNode* right = nullptr;
};

class BiomeManager {
public:
    // Mode A: WindowGrid Matrix Generator 
    // Calculates pixel coordinates for a matrix of rows and columns
    static std::vector<GridBox> GenerateWindowGrid(int screenWidth, int screenHeight, int rows, int cols, int gap);

    // Mode B: Hyprland Split Logic 
    // Takes a parent box and splits it into two equal children
    static std::pair<GridBox, GridBox> SplitBox(const GridBox& parent, SplitDirection direction, int gap);
};