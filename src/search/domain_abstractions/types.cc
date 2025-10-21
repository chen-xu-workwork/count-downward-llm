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

}

