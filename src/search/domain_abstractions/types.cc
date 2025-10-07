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
    
    // Create two new partitions
    int num_partitions = get_num_partitions();
    int new_partition_lower = num_partitions;      // for range [old_lower, n)
    int new_partition_upper = num_partitions + 1;  // for range [n, old_upper)
    
    ap_float old_lower = old_range.lower;
    ap_float old_upper = old_range.upper;
    
    // Replace the old range with the lower part [old_lower, n)
    ranges[range_index] = NumericRange(old_lower, n, new_partition_lower);
    
    // Insert the upper part [n, old_upper) after the current range
    ranges.insert(ranges.begin() + range_index + 1,
                  NumericRange(n, old_upper, new_partition_upper));
    
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

