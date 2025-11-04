#ifndef DOMAIN_ABSTRACTIONS_TYPES_H
#define DOMAIN_ABSTRACTIONS_TYPES_H

#include "../globals.h"
#include "../utils/system.h"
#include <vector>
#include <limits>
#include <memory>
#include <iostream>
#include <algorithm>

namespace domain_abstractions {
class DomainAbstraction;

// For propositional variables: maps each value to a partition index
using DomainMapping = std::vector<std::vector<int>>;

// Forward declaration for typedef
class NumericDomainMapping;

// For numeric variables: one NumericDomainMapping per numeric variable in the abstraction
// Uses unique_ptr for polymorphism (StandardSplitMapping or ExclusionSplitMapping)
using NumericDomainMappingType = std::vector<std::unique_ptr<NumericDomainMapping>>;

// For numeric variables: represents a range with configurable boundaries
// Examples: [lower, upper), (lower, upper], [lower, upper], (lower, upper)
// Note: partition_index removed - partitions are now indexed by their position in vector<Partition>
struct NumericRange {
    ap_float lower;
    ap_float upper;
    bool lower_inclusive;  // true: [lower, ...  false: (lower, ...
    bool upper_inclusive;  // true: ..., upper]  false: ..., upper)
    
    NumericRange(ap_float lower = -std::numeric_limits<ap_float>::infinity(),
                 ap_float upper = std::numeric_limits<ap_float>::infinity(),
                 bool lower_inclusive = true,
                 bool upper_inclusive = false)
        : lower(lower), upper(upper), 
          lower_inclusive(lower_inclusive), 
          upper_inclusive(upper_inclusive) {}
    
    // Check if a value is in this range
    bool contains(ap_float value) const {
        bool above_lower = lower_inclusive ? (value >= lower) : (value > lower);
        bool below_upper = upper_inclusive ? (value <= upper) : (value < upper);
        return above_lower && below_upper;
    }
    
    // Check if this range overlaps with another range specified by bounds
    bool overlaps_with(ap_float other_lower, ap_float other_upper,
                       bool other_lower_inclusive, bool other_upper_inclusive) const;
    
    // Check if this range covers the entire real line
    bool is_full_range() const {
        return lower == -std::numeric_limits<ap_float>::infinity() &&
               upper == std::numeric_limits<ap_float>::infinity();
    }
    
    // Check if this range is empty
    bool is_empty() const {
        if (lower > upper) return true;
        if (lower == upper && (!lower_inclusive || !upper_inclusive)) return true;
        return false;
    }
    
    // Check if two ranges overlap
    bool overlaps_with(const NumericRange &other) const;
    
    // Compute intersection of two ranges (may be empty)
    NumericRange intersect(const NumericRange &other) const;
};

// Partition: represents a union of disjoint numeric ranges
// Used when a single partition index corresponds to multiple non-contiguous ranges
// Example: R\{0} = (-inf, 0) ∪ (0, inf) represents two disjoint ranges
class Partition {
private:
    std::vector<NumericRange> ranges;  // Sorted by lower bound, disjoint
    
public:
    // Construct empty partition
    Partition() = default;
    
    // Construct from single range
    explicit Partition(const NumericRange &range) {
        ranges.push_back(range);
    }
    
    // Construct from multiple ranges (will be sorted and validated)
    explicit Partition(const std::vector<NumericRange> &input_ranges);
    
    // Add a range to this partition
    void add_range(const NumericRange &range);
    
    // Check if a value is in this partition (in any of its ranges)
    bool contains(ap_float value) const {
        for (const auto &range : ranges) {
            if (range.contains(value)) {
                return true;
            }
        }
        return false;
    }
    
    // Get all ranges
    const std::vector<NumericRange> &get_ranges() const {
        return ranges;
    }
    
    // Get number of ranges (number of disjoint components)
    size_t num_ranges() const {
        return ranges.size();
    }
    
    // Check if partition is empty
    bool is_empty() const {
        return ranges.empty() || std::all_of(ranges.begin(), ranges.end(),
                                            [](const NumericRange &r) { return r.is_empty(); });
    }
    
    // Get conservative bounding box [min_lower, max_upper]
    // Returns the smallest closed interval that contains all ranges
    std::pair<ap_float, ap_float> get_bounding_box() const;
    
    // Check if this partition covers the entire real line
    bool is_full_range() const;
    
    // Compute union of this partition with another
    Partition union_with(const Partition &other) const;
    
    // Compute intersection of this partition with another
    Partition intersect_with(const Partition &other) const;
    
    // Compute complement of this partition (R \ this)
    Partition complement() const;
    
    // Apply arithmetic operation to this partition with a constant value
    // op: 0=assign, 1=increase, 2=decrease, 3=scale_up, 4=scale_down
    // Returns: resulting partition after applying operation
    Partition apply_operation(f_operator op, ap_float operand) const;
    
    // Apply arithmetic operation between two partitions
    // op: 0=sum, 1=diff, 2=mult, 3=divi
    // Returns: resulting partition from operation
    static Partition apply_binary_operation(
        const Partition &left, const Partition &right, cal_operator op);
    
    // Evaluate comparison between this partition and another
    // Returns: 0=TRUE (comparison always holds), 1=FALSE (never holds), 2=UNKNOWN
    int evaluate_comparison(const Partition &other, comp_operator op) const;
    
    // Static helper: evaluate comparison between two single ranges
    // Returns: 0=TRUE, 1=FALSE, 2=UNKNOWN
    static int evaluate_range_comparison(
        comp_operator op,
        ap_float left_lower, ap_float left_upper,
        ap_float right_lower, ap_float right_upper);
    
    // Debug output
    void dump(std::ostream &out = std::cout) const;
    
    // Validate that ranges are sorted and disjoint
    bool is_valid() const;
};

// For numeric variables: each variable has a list of partitions
// Each partition contains one or more disjoint ranges
// The partitions together cover the entire real line (-inf, inf) without gaps
// Abstract base class - use StandardSplitMapping or ExclusionSplitMapping
class NumericDomainMapping {
protected:
    std::vector<Partition> partitions;
    
public:
    NumericDomainMapping() {
        // Start with a single partition covering everything (-inf, inf)
        partitions.emplace_back(NumericRange(-std::numeric_limits<ap_float>::infinity(),
                                            std::numeric_limits<ap_float>::infinity(),
                                            true, false));
    }
    
    // Virtual destructor for proper cleanup
    virtual ~NumericDomainMapping() = default;
    
    // Clone method for polymorphic copying
    virtual std::unique_ptr<NumericDomainMapping> clone() const = 0;
    
    // Get the partition index for a given value
    int get_partition_index(ap_float value) const {
        for (size_t i = 0; i < partitions.size(); ++i) {
            if (partitions[i].contains(value)) {
                return static_cast<int>(i);
            }
        }
        // Should never happen if partitions properly cover (-inf, inf)
        return -1;
    }
    
    // Split at a given value n
    // Returns the number of partitions after splitting
    // Subclasses implement different splitting strategies
    virtual int split_at(ap_float n) = 0;
    
    // Get the number of partitions
    int get_num_partitions() const {
        return static_cast<int>(partitions.size());
    }
    
    // Get all partitions (for debugging/inspection)
    const std::vector<Partition> &get_partitions() const {
        return partitions;
    }
    
    // Get a specific partition by index
    const Partition &get_partition(int partition_index) const {
        return partitions[partition_index];
    }
    
    // Get the total number of ranges across all partitions
    size_t get_total_num_ranges() const {
        size_t total = 0;
        for (const auto &partition : partitions) {
            total += partition.num_ranges();
        }
        return total;
    }
    
    // Validate internal consistency
    virtual bool is_valid() const {
        if (partitions.empty()) return false;
        
        // Check that each partition is valid
        for (const auto &partition : partitions) {
            if (!partition.is_valid()) return false;
        }
        
        // Check that all partitions together cover (-inf, inf) without gaps or overlaps
        // This is a complex check - we need to ensure union of all partitions is full range
        // and partitions don't overlap
        
        // Collect all ranges from all partitions
        std::vector<NumericRange> all_ranges;
        for (const auto &partition : partitions) {
            for (const auto &range : partition.get_ranges()) {
                all_ranges.push_back(range);
            }
        }
        
        // Sort ranges by lower bound (and inclusiveness if bounds are equal)
        std::sort(all_ranges.begin(), all_ranges.end(),
                 [](const NumericRange &a, const NumericRange &b) {
                     if (a.lower < b.lower) return true;
                     if (a.lower > b.lower) return false;
                     // If lower bounds are equal, inclusive comes before exclusive
                     return a.lower_inclusive && !b.lower_inclusive;
                 });
        
        if (all_ranges.empty()) return false;
        
        // Check first range starts at -inf
        if (all_ranges.front().lower != -std::numeric_limits<ap_float>::infinity()) {
            return false;
        }
        
        // Check last range ends at +inf
        if (all_ranges.back().upper != std::numeric_limits<ap_float>::infinity()) {
            return false;
        }
        
        // Check ranges are contiguous and don't overlap
        for (size_t i = 0; i + 1 < all_ranges.size(); ++i) {
            // Check bounds align
            if (all_ranges[i].upper != all_ranges[i+1].lower) {
                return false;  // Gap or overlap in bounds
            }
            
            // Check boundary consistency at meeting point
            if (all_ranges[i].upper_inclusive == all_ranges[i+1].lower_inclusive) {
                return false;  // Both include or both exclude the boundary
            }
        }
        
        return true;
    }
    
    // Debug method: print the partitions
    void dump() const;
    
    // Get the bounding box of a partition (min lower, max upper)
    std::pair<ap_float, ap_float> get_partition_bounding_box(int partition_index) const;
    
    // Evaluate a comparison between a partition in this mapping and a partition in another
    // Returns: 0 = TRUE, 1 = FALSE, 2 = UNKNOWN
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
    
    // Apply effect to a partition and return resulting partition indices
    // Returns: vector of partition indices that overlap with the result
    std::vector<int> apply_effect_to_partition(
        int source_partition_index, f_operator op, ap_float operand) const;
    
    // Static utility method to apply a range operation (sum, diff, mult, divi) to two ranges
    // Used for computing derived variable ranges from base variable ranges
    // Returns: pair<lower, upper> representing the result range
    static std::pair<ap_float, ap_float> apply_range_operation(
        ap_float left_lower, ap_float left_upper,
        ap_float right_lower, ap_float right_upper,
        cal_operator op);
};

// Constant mapping: represents a constant numeric variable with a single partition
// Constants never change value, so they always map to partition 0
class ConstantMapping : public NumericDomainMapping {
private:
    ap_float constant_value;
public:
    explicit ConstantMapping(ap_float value) : constant_value(value) {
        // Constants don't change - everything maps to partition 0
        // The parent constructor already added a single partition covering (-inf, inf)
    }
    
    // Constants cannot be split - just return current number of partitions (1)
    // This indicates no split occurred
    int split_at(ap_float n) override {
        std::cout << "WARNING: Attempted to split constant variable with value " 
                  << constant_value << " at " << n 
                  << " - ignoring (constants have fixed value)" << std::endl;
        return 1; // No new partition created
    }
    
    // Clone method for polymorphic copying
    std::unique_ptr<NumericDomainMapping> clone() const override {
        return std::make_unique<ConstantMapping>(*this);
    }
    
    ap_float get_constant_value() const {
        return constant_value;
    }
    
    // Override validation - constants have a single partition covering everything
    bool is_valid() const override {
        if (partitions.size() != 1) {
            return false;
        }
        // Check it's the full range (-inf, inf)
        return partitions[0].is_full_range();
    }
};

// Standard splitting strategy: splits (-inf, inf) into [(-inf, x), [x, inf)]
// Creates 2 partitions with 2 ranges
class StandardSplitMapping : public NumericDomainMapping {
public:
    // Split at point x: creates [lower, x) and [x, upper) with different partitions
    int split_at(ap_float n) override;
    
    // Clone method for polymorphic copying
    std::unique_ptr<NumericDomainMapping> clone() const override {
        return std::make_unique<StandardSplitMapping>(*this);
    }
};

// Exclusion splitting strategy: splits (-inf, inf) into [R\{x}, {x}]
// Creates 2 partitions with 3 ranges (two ranges map to same partition)
class ExclusionSplitMapping : public NumericDomainMapping {
public:
    // Split at point x: (-inf, x) and (x, inf) share one partition, [x,x] gets another
    int split_at(ap_float n) override;
    
    // Clone method for polymorphic copying
    std::unique_ptr<NumericDomainMapping> clone() const override {
        return std::make_unique<ExclusionSplitMapping>(*this);
    }
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