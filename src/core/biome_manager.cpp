#include "../../include/core/biome_manager.hpp"

// ----------------------------------------------------------------------
// MODE A: WindowGrid Matrix Generator
// ----------------------------------------------------------------------
// Divides screenWidth x screenHeight into a grid of (rows x cols) boxes,
// accounting for gap spacing between boxes.
// ----------------------------------------------------------------------
std::vector<GridBox> BiomeManager::GenerateWindowGrid(int screenWidth, int screenHeight, int rows, int cols, int gap) {
    std::vector<GridBox> grid;
    
    // Calculate total space taken up by gaps
    int totalHorizontalGaps = (cols + 1) * gap;
    int totalVerticalGaps = (rows + 1) * gap;

    // Calculate individual box width and height
    int boxWidth = (screenWidth - totalHorizontalGaps) / cols;
    int boxHeight = (screenHeight - totalVerticalGaps) / rows;

    int boxId = 0;

    // Loop through every row and column to compute bounding boxes
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            GridBox box;
            box.id = boxId++;
            box.x = gap + c * (boxWidth + gap);
            box.y = gap + r * (boxHeight + gap);
            box.width = boxWidth;
            box.height = boxHeight;
            box.assignedAppTitle = "";

            grid.push_back(box);
        }
    }

    return grid;
}

// ----------------------------------------------------------------------
// MODE B: Hyprland Split Logic (Binary Partitioning)
// ----------------------------------------------------------------------
// Takes a parent GridBox and cuts it 50/50 vertically or horizontally.
// ----------------------------------------------------------------------
std::pair<GridBox, GridBox> BiomeManager::SplitBox(const GridBox& parent, SplitDirection direction, int gap) {
    // pair is a statndard library container that holds exactly two values 
    GridBox child1 = parent;
    GridBox child2 = parent;

    if (direction == SplitDirection::VERTICAL) {
        // Cut width in half (minus half the gap)
        int newWidth = (parent.width - gap) / 2;
        
        // Left Box
        child1.width = newWidth;
        
        // Right Box
        child2.x = parent.x + newWidth + gap;
        child2.width = newWidth;
    } 
    else if (direction == SplitDirection::HORIZONTAL) {
        // Cut height in half (minus half the gap)
        int newHeight = (parent.height - gap) / 2;

        // Top Box
        child1.height = newHeight;

        // Bottom Box
        child2.y = parent.y + newHeight + gap;
        child2.height = newHeight;
    }

    return { child1, child2 };
}