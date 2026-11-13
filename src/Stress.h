#pragma once
#include <cstddef>
#include "Board.h"

// Debug helper: fills the board with random handwriting-like strokes around `center`
// until about `targetPoints` points have been added. Deterministic (fixed seed per call).
void AddStressStrokes(Board& board, Vec2d center, double halfExtent, size_t targetPoints, unsigned seed);
