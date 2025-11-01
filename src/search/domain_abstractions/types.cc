#include "types.h"

#include "../utils/hash.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace domain_abstractions {

int NumericDomainMapping::split_at(ap_float n) {
    // Find the range that contains n
    int range_index = -1;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].contains(n)) {
            range_index = i;
            break;
        }
    }
    
    // If n is not in any range or is already at a boundary, do nothing
    if (range_index == -1) {
        return get_num_partitions();
    }
    
    NumericRange &old_range = ranges[range_index];
    
    // If n is already at the lower bound, no split needed
    if (old_range.lower == n) {
        return get_num_partitions();
    }
    
    // Split the range into two parts:
    // Keep the old partition index for the lower part [old_lower, n)
    // Create a new partition index for the upper part [n, old_upper)
    int num_partitions = get_num_partitions();
    int old_partition = old_range.partition_index;  // Reuse for lower part
    int new_partition = num_partitions;             // New index for upper part
    
    ap_float old_lower = old_range.lower;
    ap_float old_upper = old_range.upper;
    
    // Replace the old range with the lower part [old_lower, n), keeping old partition index
    ranges[range_index] = NumericRange(old_lower, n, old_partition);
    
    // Insert the upper part [n, old_upper) with new partition index
    ranges.insert(ranges.begin() + range_index + 1,
                  NumericRange(n, old_upper, new_partition));
    
    return get_num_partitions();
}

void NumericDomainMapping::dump() const {
    std::cout << "NumericDomainMapping with " << get_num_partitions() 
              << " partitions and " << get_num_ranges() << " ranges:" << std::endl;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto &range = ranges[i];
        std::cout << "  Range " << i << ": [";
        if (range.lower == -std::numeric_limits<ap_float>::infinity()) {
            std::cout << "-inf";
        } else {
            std::cout << range.lower;
        }
        std::cout << ", ";
        if (range.upper == std::numeric_limits<ap_float>::infinity()) {
            std::cout << "inf";
        } else {
            std::cout << range.upper;
        }
        std::cout << ") -> partition " << range.partition_index << std::endl;
    }
}

const NumericRange* NumericDomainMapping::get_range_for_partition(int partition_index) const {
    for (const auto &range : ranges) {
        if (range.partition_index == partition_index) {
            return &range;
        }
    }
    return nullptr;
}

std::pair<ap_float, ap_float> NumericDomainMapping::get_range_union() const {
    if (ranges.empty()) {
        return {-std::numeric_limits<ap_float>::infinity(),
                std::numeric_limits<ap_float>::infinity()};
    }
    
    ap_float min_lower = std::numeric_limits<ap_float>::infinity();
    ap_float max_upper = -std::numeric_limits<ap_float>::infinity();
    
    for (const auto &range : ranges) {
        min_lower = std::min(min_lower, range.lower);
        max_upper = std::max(max_upper, range.upper);
    }
    
    return {min_lower, max_upper};
}

int NumericDomainMapping::evaluate_comparison(
    comp_operator op,
    ap_float left_lower, ap_float left_upper,
    ap_float right_lower, ap_float right_upper) {
    
    // Evaluate based on comparison operator
    // Returns: 0 = definitely false, 1 = definitely true, 2 = unknown
    
    switch (op) {
        case comp_operator::lt: // left < right
            // Definitely true if: max(left) < min(right), i.e., left_upper <= right_lower
            // Definitely false if: min(left) >= max(right), i.e., left_lower >= right_upper
            if (left_upper <= right_lower) {
                return 1; // definitely true
            } else if (left_lower >= right_upper) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case comp_operator::le: // left <= right
            // Definitely true if: max(left) <= min(right)
            // Definitely false if: min(left) > max(right)
            if (left_upper <= right_lower) {
                return 1; // definitely true
            } else if (left_lower > right_upper) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case comp_operator::eq: // left == right
            // Definitely true only if both ranges are the same single point
            if (left_lower == left_upper && right_lower == right_upper && 
                left_lower == right_lower) {
                return 1; // definitely true (both are same point)
            }
            // Definitely false if ranges don't overlap
            else if (left_upper <= right_lower || right_upper <= left_lower) {
                return 0; // definitely false (no overlap)
            } else {
                return 2; // unknown
            }
            
        case comp_operator::ge: // left >= right
            // Definitely true if: min(left) >= max(right)
            // Definitely false if: max(left) < min(right)
            if (left_lower >= right_upper) {
                return 1; // definitely true
            } else if (left_upper < right_lower) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        case comp_operator::gt: // left > right
            // Definitely true if: min(left) >= max(right), i.e., left_lower >= right_upper
            // Definitely false if: max(left) <= min(right), i.e., left_upper <= right_lower
            if (left_lower >= right_upper) {
                return 1; // definitely true
            } else if (left_upper <= right_lower) {
                return 0; // definitely false
            } else {
                return 2; // unknown
            }
            
        default:
            return 2; // unknown for unrecognized operators
    }
}

int NumericDomainMapping::evaluate_comparison_with(
    const NumericDomainMapping &other,
    int my_partition,
    int other_partition,
    comp_operator op) const {
    
    // Look up ranges for both partitions
    const NumericRange *my_range = get_range_for_partition(my_partition);
    const NumericRange *other_range = other.get_range_for_partition(other_partition);
    
    if (!my_range || !other_range) {
        return 2; // unknown if partition not found
    }
    
    // Use the static evaluation method
    return evaluate_comparison(op,
                              my_range->lower, my_range->upper,
                              other_range->lower, other_range->upper);
}

}

