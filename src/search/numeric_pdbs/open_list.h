#ifndef INNER_OPEN_LIST
#define INNER_OPEN_LIST

#include <iostream>      // Required for input/output operations (e.g., std::cout)
#include <queue>         // For std::priority_queue
#include <vector>        // Default underlying container for std::priority_queue
#include <string>        // For std::string in NodeId
#include <variant>       // For std::variant

// Define ap_float if it's not globally defined. Using double for generality.
typedef double ap_float;

// Forward declaration of NodeValue type for the example
// using NodeId = std::string; // Example usage if NodeValue is string

// --- FValue Class (for A* search) ---
// Implements A* specific cost logic.
class FValue {
private:
    ap_float g_cost; // Cost from start to current node
    ap_float h_cost; // Heuristic cost from current node to goal

public:
    // Constructor to initialize costs
    FValue(ap_float g, ap_float h) : g_cost(g), h_cost(h) {}

    ap_float get_g() const { return g_cost; }
    ap_float get_f() const { return g_cost + h_cost; }
    ap_float get_h() const { return h_cost; }

    // A* specific comparison logic (lower f, then lower h for tie-breaking)
    bool compare_less(const FValue& other) const {
        ap_float f1 = get_f();
        ap_float f2 = other.get_f();

        if (f1 < f2) {
            return true; // Current FValue has a strictly smaller total cost, so it's "less" (higher priority)
        } else if (f1 == f2) {
            // If total costs are equal, use 'h' as a tie-breaker.
            // A smaller 'h' is generally preferred in A* to explore paths closer to the goal.
            return h_cost < other.h_cost; // Current FValue has smaller 'h', so it's "less" (higher priority)
        }
        return false; // Current FValue is greater or equal in priority
    }
};

// --- GValue Class (for Blind search, e.g., Dijkstra/BFS) ---
// Only considers the g-cost.
class GValue {
private:
    ap_float g_cost; // Cost from start to current node

public:
    // Constructor takes same arguments as FValue but ignores 'h' for compatibility.
    GValue(ap_float g, ap_float /*h_ignored_for_compatibility*/) : g_cost(g) {}

    ap_float get_g() const { return g_cost; }
    // For blind search, f-value is conceptually the same as g-value (h is 0)
    ap_float get_f() const { return g_cost; }
    ap_float get_h() const { return 0; } // GValue has no heuristic component

    // Blind search specific comparison logic (lower g is better)
    bool compare_less(const GValue& other) const {
        return g_cost < other.g_cost; // Smaller 'g' means higher priority
    }
};


// --- Entry Class for the Open List ---
// Combines the cost information (polymorphically via AbstractCost)
// with the actual 'NodeValue' representing the node's data.
template <typename NodeValue>
class Entry {
public:
    // Using std::variant to store either FValue or GValue directly.
    std::variant<FValue, GValue> cost;
    NodeValue data; // The actual data associated with this entry

    // Constructor for an Entry
    template<typename CostType>
    Entry(CostType c, NodeValue d)
        : cost(c), data(d) {}

    // Get the g-cost. Uses std::visit to call the appropriate get_g() method.
    ap_float get_g() const {
        return std::visit([](const auto& c){ return c.get_g(); }, cost);
    }

    // Get the f-cost. Uses std::visit to call the appropriate get_f() method.
    ap_float get_f() const {
        return std::visit([](const auto& c){ return c.get_f(); }, cost);
    }

    ap_float get_h() const {
        return std::visit([](const auto& c){ return c.get_h(); }, cost);
    }

    // Overload the less than operator (<) for Entry.
    // This is crucial for std::priority_queue, which is a max-heap by default.
    // We want the Entry with the *smallest* cost (highest priority) to be at the top of the heap.
    // To achieve this with a max-heap, this operator should return true if 'this' Entry
    // has *lower* priority than 'other' Entry.
    bool operator<(const Entry<NodeValue>& other) const {
        // The OpenList's push method guarantees that both variants hold the same type.
        // We can safely use std::get to access the concrete type.
        if (std::holds_alternative<FValue>(cost)) {
            return std::get<FValue>(other.cost).compare_less(std::get<FValue>(cost));
        } else {
            return std::get<GValue>(other.cost).compare_less(std::get<GValue>(cost));
        }
    }
};

// --- Merged OpenList Class ---
// A single class for both A* and Blind search, controlled by a flag.
template <typename NodeValue>
class OpenList {
private:
    std::priority_queue<Entry<NodeValue>> pq;
    bool ignore_h_value; // Flag to determine if 'h' should be ignored (for blind search)

public:
    // Constructor: Set `ignore_h` to true for blind search, false for A* (default)
    OpenList(bool ignore_h = false) : ignore_h_value(ignore_h) {}

    // Pushes a new entry to the open list.
    // Dynamically creates either an FValue or GValue object based on `ignore_h_value`.
    void push(ap_float g, ap_float h, const NodeValue& data) {
        if (ignore_h_value) {
            // For blind search, create a GValue object. 'h' is passed but ignored by GValue's constructor.
            pq.push(Entry<NodeValue>(GValue(g, h), data));
        } else {
            // For A* search, create an FValue object.
            pq.push(Entry<NodeValue>(FValue(g, h), data));
        }
    }

    // Returns the top (highest priority) entry without removing it.
    Entry<NodeValue> top() const {
        return pq.top(); // Returns a copy; shared_ptr ensures cost object is correctly shared
    }

    // Removes the top entry.
    void pop() {
        pq.pop();
    }

    // Checks if the open list is empty.
    bool empty() const {
        return pq.empty();
    }

    // Returns the number of elements in the open list.
    size_t size() const {
        return pq.size();
    }
};

#endif