#ifndef INNER_OPEN_LIST
#define INNER_OPEN_LIST

#include <iostream>      // Required for input/output operations (e.g., std::cout)
#include <queue>         // For std::priority_queue
#include <vector>        // Default underlying container for std::priority_queue
#include <string>        // For std::string in NodeId

// --- A* Open List Components ---

// FValue struct encapsulates the g, h, and f costs for A* pathfinding.
// It defines the primary comparison logic for nodes in the open list.
struct FValue {
    int g; // Cost from start to current node
    int h; // Heuristic cost from current node to goal
    int f; // Total estimated cost: g + h

    // Constructor to initialize costs
    FValue(int g_cost, int h_cost) : g(g_cost), h(h_cost), f(g_cost + h_cost) {}

    // Overload the less than operator (<) for FValue comparison.
    // This defines the ordering for A* search:
    // 1. Primary sort key: lower 'f' is better (higher priority).
    // 2. Tie-breaker: if 'f' values are equal, lower 'h' is better (higher priority).
    bool operator<(const FValue& other) const {
        if (f < other.f) {
            return true; // Current FValue has a strictly smaller total cost, so it's "less" (higher priority)
        } else if (f == other.f) {
            // If total costs are equal, use 'h' as a tie-breaker.
            // A smaller 'h' is generally preferred in A* to explore paths closer to the goal.
            return h < other.h; // Current FValue has smaller 'h', so it's "less" (higher priority)
        }
        return false; // Current FValue is greater or equal in priority
    }

    // Overload the equality operator (==) for FValue.
    // Two FValues are considered equal if both their 'f' and 'h' values are the same.
    bool operator==(const FValue& other) const {
        return (f == other.f) && (h == other.h);
    }
};

// Entry struct for the A* open list.
// It combines the FValue (for priority ordering) with the actual 'Value'
// representing the node's data (e.g., coordinates, state, node ID).
template <typename Value>
struct Entry {
    FValue f_value; // The A* cost information (g, h, f)
    Value data;     // The actual data associated with this entry (e.g., Node ID, coordinates)

    // Constructor for an Entry
    Entry(FValue fv, Value d) : f_value(fv), data(d) {}

    // Overload the less than operator (<) for Entry.
    // This is crucial for std::priority_queue, which is a max-heap by default.
    // We want the Entry with the *smallest* FValue (i.e., highest priority) to be at the top of the heap.
    // To achieve this with a max-heap, this operator should return true if 'this' Entry
    // has *lower* priority than 'other' Entry.
    // 'this' has lower priority if 'this.f_value' is "greater" than 'other.f_value'
    // according to our A* ordering (defined by FValue::operator<).
    bool operator<(const Entry<Value>& other) const {
        // We want the Entry with the *smallest* FValue to have the *highest* priority.
        // So, if 'other.f_value' is "less" than 'this.f_value' (meaning 'other' has higher priority),
        // then 'this' has lower priority and should be considered "less" for the max-heap.
        return other.f_value < f_value; // This effectively inverts the comparison, making it a min-heap
    }
};

template <typename Value>
using AStarOpenList = std::priority_queue<Entry<Value>>;

#endif