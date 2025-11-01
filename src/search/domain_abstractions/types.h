#ifndef DOMAIN_ABSTRACTIONS_TYPES_H
#define DOMAIN_ABSTRACTIONS_TYPES_H

#include "../globals.h"
#include <vector>
#include <limits>

namespace domain_abstractions {
class DomainAbstraction;

// For propositional variables: maps each value to a partition index
using DomainMapping = std::vector<std::vector<int>>;

// Forward declaration for typedef
class NumericDomainMapping;

// For numeric variables: one NumericDomainMapping per numeric variable in the abstraction
using NumericDomainMappingType = std::vector<NumericDomainMapping>;

// For numeric variables: represents a range [lower, upper)
struct NumericRange {
    ap_float lower;  // inclusive
    ap_float upper;  // exclusive
    int partition_index;  // which partition this range belongs to
    
    NumericRange(ap_float lower = -std::numeric_limits<ap_float>::infinity(),
                 ap_float upper = std::numeric_limits<ap_float>::infinity(),
                 int partition_index = 0)
        : lower(lower), upper(upper), partition_index(partition_index) {}
    
    // Check if a value is in this range
    bool contains(ap_float value) const {
        return value >= lower && value < upper;
    }
    
    // Check if this range covers the entire real line
    bool is_full_range() const {
        return lower == -std::numeric_limits<ap_float>::infinity() &&
               upper == std::numeric_limits<ap_float>::infinity();
    }
};

// For numeric variables: each variable has a list of ranges that partition (-inf, inf)
// The ranges must be sorted by lower bound and cover the entire real line without gaps
class NumericDomainMapping {
    std::vector<NumericRange> ranges;
    
public:
    NumericDomainMapping() {
        // Start with a single range covering everything, partition 0
        ranges.emplace_back(-std::numeric_limits<ap_float>::infinity(),
                           std::numeric_limits<ap_float>::infinity(),
                           0);
    }
    
    // Get the partition index for a given value
    int get_partition_index(ap_float value) const {
        for (const auto &range : ranges) {
            if (range.contains(value)) {
                return range.partition_index;
            }
        }
        // Should never happen if ranges properly cover (-inf, inf)
        return -1;
    }
    
    // Split at a given value n: creates ranges (..., n) and [n, ...)
    // Returns the number of partitions after splitting
    int split_at(ap_float n);
    
    // Get the number of partitions (max partition index + 1)
    int get_num_partitions() const {
        int max_partition = 0;
        for (const auto &range : ranges) {
            if (range.partition_index > max_partition) {
                max_partition = range.partition_index;
            }
        }
        return max_partition + 1;
    }
    
    // Get all ranges (for debugging/inspection)
    const std::vector<NumericRange> &get_ranges() const {
        return ranges;
    }
    
    // Get the number of ranges
    size_t get_num_ranges() const {
        return ranges.size();
    }
    
    // Validate internal consistency
    bool is_valid() const {
        if (ranges.empty()) return false;
        
        // Check that ranges are sorted and contiguous
        for (size_t i = 0; i + 1 < ranges.size(); ++i) {
            if (ranges[i].upper != ranges[i+1].lower) {
                return false;  // Gap or overlap
            }
            if (ranges[i].lower >= ranges[i].upper) {
                return false;  // Invalid range
            }
        }
        
        // Check first range starts at -inf and last ends at +inf
        if (ranges.front().lower != -std::numeric_limits<ap_float>::infinity()) {
            return false;
        }
        if (ranges.back().upper != std::numeric_limits<ap_float>::infinity()) {
            return false;
        }
        
        return true;
    }
    
    // Debug method: print the ranges
    void dump() const;
    
    // Get the range associated with a given partition index
    // Returns nullptr if no range exists for this partition
    const NumericRange* get_range_for_partition(int partition_index) const;
    
    // Get the union of all ranges (returns min of all lowers, max of all uppers)
    std::pair<ap_float, ap_float> get_range_union() const;
    
    // Static method to evaluate a comparison between two ranges
    // Returns: 0 = definitely false, 1 = definitely true, 2 = unknown
    static int evaluate_comparison(
        comp_operator op,
        ap_float left_lower, ap_float left_upper,
        ap_float right_lower, ap_float right_upper);
    
    // Evaluate a comparison between a partition in this mapping and a partition in another
    // Returns: 0 = definitely false, 1 = definitely true, 2 = unknown
    int evaluate_comparison_with(
        const NumericDomainMapping &other,
        int my_partition,
        int other_partition,
        comp_operator op) const;
    
    // Compute which partitions are reachable from a source partition after applying an effect
    // Uses forward/progression semantics to determine reachability
    // Returns: vector of partition indices that can be reached
    std::vector<int> compute_reachable_partitions(
        int source_partition,
        f_operator effect_op,
        ap_float operand_value) const;
    
    // Static method to apply a range operation (sum, diff, mult, divi) to two ranges
    // Used for computing derived variable ranges from base variable ranges
    // Returns: pair<lower, upper> representing the result range
    static std::pair<ap_float, ap_float> apply_range_operation(
        ap_float left_lower, ap_float left_upper,
        ap_float right_lower, ap_float right_upper,
        cal_operator op);
};

// Domain Abstraction State
// Since numeric variables are discretized into partitions (finite discrete values),
// we can treat the entire state (propositional + discretized numeric) uniformly.
// We just need a single hash that combines both propositional and numeric components.
struct DomainAbstractionState {
    size_t state_hash;  // Combined hash for propositional and numeric variables
    
    explicit DomainAbstractionState(size_t state_hash)
        : state_hash(state_hash) {}
    
    bool operator==(const DomainAbstractionState &other) const {
        return state_hash == other.state_hash;
    }
};

// Hash function for DomainAbstractionState
struct DomainAbstractionStateHash {
    std::size_t operator()(const DomainAbstractionState &s) const {
        return s.state_hash;
    }
};

using DomainAbstractionCollection = std::vector<DomainAbstraction>;
}

#endif