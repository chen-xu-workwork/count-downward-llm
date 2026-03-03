#ifndef DOMAIN_ABSTRACTIONS_TYPES_H
#define DOMAIN_ABSTRACTIONS_TYPES_H

#include "../globals.h"
#include "../utils/system.h"
#include <vector>
#include <limits>
#include <memory>
#include <string>
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
using NumericDomainMappings = std::vector<std::unique_ptr<NumericDomainMapping>>;

// For numeric variables: represents a range with configurable boundaries
// Examples: [lower, upper), (lower, upper], [lower, upper], (lower, upper)
struct NumericRange {
    ap_float lower;
    ap_float upper;
    bool lower_inclusive;  // true: [lower, ...  false: (lower, ...
    bool upper_inclusive;  // true: ..., upper]  false: ..., upper)
    int partition_index;   // which partition this range belongs to
    
    NumericRange(ap_float lower = -std::numeric_limits<ap_float>::infinity(),
                 ap_float upper = std::numeric_limits<ap_float>::infinity(),
                 bool lower_inclusive = true,
                 bool upper_inclusive = false,
                 int partition_index = 0)
        : lower(lower), upper(upper), 
          lower_inclusive(lower_inclusive), 
          upper_inclusive(upper_inclusive),
          partition_index(partition_index) {}
    
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

    std::string to_string() const;

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
    
    // Debug output
    void dump(std::ostream &out = std::cout) const;

    // Validate that ranges are sorted and disjoint
    bool is_valid() const;
};

// For numeric variables: each variable has a list of ranges that partition (-inf, inf)
// The ranges must be sorted by lower bound and cover the entire real line without gaps
// Abstract base class - use StandardSplitMapping or ExclusionSplitMapping
class NumericDomainMapping {
protected:
    std::vector<NumericRange> ranges;
    
    // Lookup table: partition_index -> index into ranges vector.
    // Mutable because it's a cache that can be rebuilt from ranges.
    mutable std::vector<int> partition_to_range_idx;
    mutable bool partition_lookup_valid = false;
    
    // Rebuild the partition lookup table from ranges.
    void rebuild_partition_lookup() const {
        if (ranges.empty()) {
            partition_to_range_idx.clear();
            partition_lookup_valid = true;
            return;
        }
        // Find max partition index
        int max_partition = 0;
        for (const auto &range : ranges) {
            if (range.partition_index > max_partition) {
                max_partition = range.partition_index;
            }
        }
        partition_to_range_idx.assign(max_partition + 1, -1);
        for (size_t i = 0; i < ranges.size(); ++i) {
            partition_to_range_idx[ranges[i].partition_index] = static_cast<int>(i);
        }
        partition_lookup_valid = true;
    }
    
    // Invalidate the lookup (call after modifying ranges).
    void invalidate_partition_lookup() {
        partition_lookup_valid = false;
    }
    
public:
    NumericDomainMapping() {
        ranges.emplace_back(-std::numeric_limits<ap_float>::infinity(),
                           std::numeric_limits<ap_float>::infinity(),
                           0);
        invalidate_partition_lookup();
    }
    
    virtual ~NumericDomainMapping() = default;
    
    // Clone method for polymorphic copying
    virtual std::unique_ptr<NumericDomainMapping> clone() const = 0;

    virtual bool can_split(ap_float value, bool include_in_lower) const = 0;
    
    int get_partition_index(ap_float value) const {
        if (ranges.empty()) return -1;
        
        // Binary search: find the last range whose lower <= value
        size_t lo = 0, hi = ranges.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (ranges[mid].lower <= value) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        // lo is now the first range with lower > value, so check lo-1
        if (lo > 0) {
            const NumericRange &candidate = ranges[lo - 1];
            if (candidate.contains(value)) {
                return candidate.partition_index;
            }
        }
        // Fallback: linear search (shouldn't happen with proper ranges)
        for (const auto &range : ranges) {
            if (range.contains(value)) {
                return range.partition_index;
            }
        }
        return -1;
    }
    
    // Split at a given value n
    // Returns the number of partitions after splitting
    // Subclasses implement different splitting strategies
    // For StandardSplitMapping, include_in_lower determines if n is in lower range:
    //   include_in_lower=true:  [lower, n] and (n, upper)
    //   include_in_lower=false: [lower, n) and [n, upper) (default, current behavior)
    // For other strategies (Exclusion, Constant), the parameter is ignored.
    virtual int split_at(ap_float n, bool include_in_lower = false) = 0;
    
    // Get the number of partitions (max partition index + 1)
    int get_num_partitions() const {
        if (this == nullptr) {
            std::cerr << "CRITICAL ERROR: get_num_partitions called on nullptr!" << std::endl;
            utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        }
        // Use cached lookup if valid
        if (partition_lookup_valid) {
            return static_cast<int>(partition_to_range_idx.size());
        }
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
    virtual bool is_valid() const {
        if (ranges.empty()) return false;
        
        // Check that ranges are sorted and contiguous
        for (size_t i = 0; i + 1 < ranges.size(); ++i) {
            // Check bounds align
            if (ranges[i].upper != ranges[i+1].lower) {
                return false;  // Gap or overlap in bounds
            }
            
            // Check boundary consistency at meeting point
            // Adjacent ranges must not both include the boundary point
            // (would cause overlap) or both exclude it (would cause gap)
            if (ranges[i].upper_inclusive == ranges[i+1].lower_inclusive) {
                return false;  // Both include or both exclude the boundary
            }
            
            // Check range is not empty (lower < upper, or lower == upper with both inclusive)
            if (ranges[i].lower > ranges[i].upper) {
                return false;  // Invalid range
            }
            if (ranges[i].lower == ranges[i].upper) {
                // Single point range - must have both boundaries inclusive
                if (!ranges[i].lower_inclusive || !ranges[i].upper_inclusive) {
                    return false;  // Empty range
                }
            }
        }
        
        // Check last range validity
        if (ranges.back().lower > ranges.back().upper) {
            return false;
        }
        if (ranges.back().lower == ranges.back().upper) {
            if (!ranges.back().lower_inclusive || !ranges.back().upper_inclusive) {
                return false;
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

    static int evaluate_comparison(
        comp_operator op,
        const NumericRange &left, const NumericRange &right);
    
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

    static NumericRange apply_range_operation(
        const NumericRange &left, const NumericRange &right, cal_operator op);
    
    // ========================================================================
    // Partition Integration Methods
    // ========================================================================
    
    // Get a Partition object representing all ranges for a given partition index
    // Returns: Partition containing all ranges that map to the given partition index
    Partition get_partition(int partition_index) const;
    
    // Get bounding box for a partition index (conservative bounds)
    // Returns: pair<lower, upper> representing the smallest interval containing the partition
    std::pair<ap_float, ap_float> get_partition_bounding_box(int partition_index) const;
    
    // Check if a value belongs to a specific partition
    bool value_in_partition(ap_float value, int partition_index) const {
        return get_partition_index(value) == partition_index;
    }
    
    // Get all partition indices (sorted, unique)
    std::vector<int> get_all_partition_indices() const;
    
    // Evaluate comparison between two partitions from different mappings
    // Returns: 0=TRUE, 1=FALSE, 2=UNKNOWN
    static int evaluate_partition_comparison(
        const Partition &left, const Partition &right, comp_operator op);
    
    // Apply an effect operation to a partition and determine resulting partition indices
    // Returns: vector of partition indices that the result could map to
    std::vector<int> apply_effect_to_partition(
        int source_partition_index, f_operator op, ap_float operand) const;
};

// Constant mapping: represents a constant numeric variable with a single partition
// Constants never change value, so they always map to partition 0
class ConstantMapping : public NumericDomainMapping {
private:
    ap_float constant_value;
public:
    explicit ConstantMapping(ap_float value) : constant_value(value) {
        // Constants don't change - everything maps to partition 0
        // The parent constructor already added the full range (-inf, inf) with partition 0
        // So we don't need to modify it!
    }
    
    // Constants cannot be split - just return current number of partitions (1)
    // This indicates no split occurred
    // include_in_lower parameter is ignored for constants
    int split_at(ap_float n, bool /*include_in_lower*/ = false) override {
        std::cout << "Attempt spliiting at contant variable " << std::endl;
        utils::exit_with(utils::ExitCode::CRITICAL_ERROR);
        return 1;  // No split - still just 1 partition}
    }

    bool can_split(ap_float /*value*/, bool /*include_in_lower*/) const override {
        return false;  // Cannot split a constant variable
    }
    
    // Clone method for polymorphic copying
    std::unique_ptr<NumericDomainMapping> clone() const override {
        return std::make_unique<ConstantMapping>(*this);
    }
    
    ap_float get_constant_value() const {
        return constant_value;
    }
    
    // Constants always have exactly 1 partition
    int get_num_partitions() const {
        return 1;
    }
    
    // Override validation - constants have a single range covering everything with partition 0
    bool is_valid() const override {
        if (ranges.size() != 1) {
            return false;
        }
        // Check it's the full range (-inf, inf) with partition 0
        return ranges[0].lower == -std::numeric_limits<ap_float>::infinity() &&
               ranges[0].upper == std::numeric_limits<ap_float>::infinity() &&
               ranges[0].partition_index == 0;
    }
};

// Standard splitting strategy: splits (-inf, inf) into [(-inf, x), [x, inf)]
// Creates 2 partitions with 2 ranges
class StandardSplitMapping : public NumericDomainMapping {
    std::vector<std::pair<ap_float, bool>> split_points;  // pair<split_value, include_in_lower>
public:
    // Split at point x: creates [lower, x) and [x, upper) with different partitions
    // If include_in_lower=true: creates [lower, x] and (x, upper) instead
    int split_at(ap_float n, bool include_in_lower = false) override;
    
    std::unique_ptr<NumericDomainMapping> clone() const override {
        return std::make_unique<StandardSplitMapping>(*this);
    }

    bool can_split(ap_float value, bool include_in_lower) const override {
        std::pair<ap_float, bool> split_point = {value, include_in_lower};
        return std::find(split_points.begin(), split_points.end(), split_point) == split_points.end();
    }
};

// Exclusion splitting strategy: splits (-inf, inf) into [R\{x}, {x}]
// Creates 2 partitions with 3 ranges (two ranges map to same partition)
class ExclusionSplitMapping : public NumericDomainMapping {
public:
    // Split at point x: (-inf, x) and (x, inf) share one partition, [x,x] gets another
    // include_in_lower parameter is ignored for exclusion strategy
    int split_at(ap_float n, bool include_in_lower = false) override;

    bool can_split(ap_float value, bool /*include_in_lower*/) const override {
        // Can split if value is not already a split point (i.e., not already isolated in its own partition)
        for (const auto &range : ranges) {
            if (range.contains(value) && range.lower == range.upper) {
                return false;  // Already a split point
            }
        }
        std::cout << "BRUH" << std::endl;
        return true;
    }
    
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