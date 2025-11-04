#include "types.h"

#include "../utils/hash.h"

#include <algorithm>
#include <cassert>
#include <iostream>

namespace domain_abstractions {

bool NumericRange::overlaps_with(ap_float other_lower, ap_float other_upper,
                                   bool other_lower_inclusive, bool other_upper_inclusive) const {
    // Two ranges overlap if they share at least one point
    // This is complex with open/closed boundaries
    
    // First, check if ranges are disjoint (no overlap)
    // Range 1: this range, Range 2: other range
    
    // Case 1: This range is entirely below other range
    // this.upper compared to other.lower
    if (upper < other_lower) {
        return false;  // Definitely disjoint
    }
    if (upper == other_lower) {
        // They touch at a point - overlap only if both include that point
        return upper_inclusive && other_lower_inclusive;
    }
    
    // Case 2: This range is entirely above other range
    // this.lower compared to other.upper
    if (lower > other_upper) {
        return false;  // Definitely disjoint
    }
    if (lower == other_upper) {
        // They touch at a point - overlap only if both include that point
        return lower_inclusive && other_upper_inclusive;
    }
    
    // If we get here, the ranges overlap
    return true;
}

// StandardSplitMapping implementation
int StandardSplitMapping::split_at(ap_float n) {
    // Find which partition contains n
    int partition_index = get_partition_index(n);
    
    if (partition_index == -1) {
        // Value not in any partition (shouldn't happen)
        return get_num_partitions();
    }
    
    Partition &old_partition = partitions[partition_index];
    
    // Find which range within this partition contains n
    const std::vector<NumericRange> &ranges = old_partition.get_ranges();
    int range_idx = -1;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].contains(n)) {
            range_idx = static_cast<int>(i);
            break;
        }
    }
    
    if (range_idx == -1) {
        // Value not found in partition ranges (shouldn't happen)
        return get_num_partitions();
    }
    
    const NumericRange &old_range = ranges[range_idx];
    
    // If n is already at the lower bound, no split needed
    if (old_range.lower == n) {
        return get_num_partitions();
    }
    
    // Create two new partitions from the split
    // Lower partition: [old_lower, n)
    // Upper partition: [n, old_upper)
    
    ap_float old_lower = old_range.lower;
    ap_float old_upper = old_range.upper;
    bool old_lower_inclusive = old_range.lower_inclusive;
    bool old_upper_inclusive = old_range.upper_inclusive;
    
    // Build lower partition - contains all ranges from old partition up to n
    Partition lower_partition;
    for (int i = 0; i < range_idx; ++i) {
        lower_partition.add_range(ranges[i]);
    }
    // Add the truncated range: [old_lower, n)
    lower_partition.add_range(NumericRange(old_lower, n, old_lower_inclusive, false));
    
    // Build upper partition - contains the split range and all subsequent ranges
    Partition upper_partition;
    // Add the split range: [n, old_upper)
    upper_partition.add_range(NumericRange(n, old_upper, true, old_upper_inclusive));
    for (size_t i = range_idx + 1; i < ranges.size(); ++i) {
        upper_partition.add_range(ranges[i]);
    }
    
    // Replace old partition with lower partition, add upper partition
    partitions[partition_index] = lower_partition;
    partitions.push_back(upper_partition);
    
    return get_num_partitions();
}

// ExclusionSplitMapping implementation
int ExclusionSplitMapping::split_at(ap_float n) {
    // Find which partition contains n
    int partition_index = get_partition_index(n);
    
    if (partition_index == -1) {
        // Value not in any partition (shouldn't happen)
        return get_num_partitions();
    }
    
    Partition &old_partition = partitions[partition_index];
    
    // Find which range within this partition contains n
    const std::vector<NumericRange> &ranges = old_partition.get_ranges();
    int range_idx = -1;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].contains(n)) {
            range_idx = static_cast<int>(i);
            break;
        }
    }
    
    if (range_idx == -1) {
        // Value not found in partition ranges (shouldn't happen)
        return get_num_partitions();
    }
    
    const NumericRange &old_range = ranges[range_idx];
    
    // If n is already at a boundary, don't split
    if (old_range.lower == n || old_range.upper == n) {
        return get_num_partitions();
    }
    
    // Create two new partitions:
    // Exclusion partition: R\{n} (all ranges except the point)
    // Point partition: {n} (single point)
    
    ap_float old_lower = old_range.lower;
    ap_float old_upper = old_range.upper;
    bool old_lower_inclusive = old_range.lower_inclusive;
    bool old_upper_inclusive = old_range.upper_inclusive;
    
    // Build exclusion partition - contains all old ranges plus split ranges, minus the point
    Partition exclusion_partition;
    
    // Add all ranges before the range containing n
    for (int i = 0; i < range_idx; ++i) {
        exclusion_partition.add_range(ranges[i]);
    }
    
    // Add lower part of split range: [old_lower, n)
    exclusion_partition.add_range(NumericRange(old_lower, n, old_lower_inclusive, false));
    
    // Add upper part of split range: (n, old_upper)
    exclusion_partition.add_range(NumericRange(n, old_upper, false, old_upper_inclusive));
    
    // Add all ranges after the range containing n
    for (size_t i = range_idx + 1; i < ranges.size(); ++i) {
        exclusion_partition.add_range(ranges[i]);
    }
    
    // Build point partition - contains only the single point [n, n]
    Partition point_partition(NumericRange(n, n, true, true));
    
    // CRITICAL: Don't replace the old partition! That would invalidate existing state encodings.
    // Instead, keep the old partition index pointing to the exclusion part,
    // and append the point partition as a new index.
    // This maintains semantic compatibility: values that were in partition_index before
    // and are NOT the split point remain in partition_index.
    partitions[partition_index] = exclusion_partition;
    partitions.push_back(point_partition);
    
    return get_num_partitions();
}

void NumericDomainMapping::dump() const {
    std::cout << "NumericDomainMapping with " << get_num_partitions() 
              << " partitions:" << std::endl;
    for (size_t part_idx = 0; part_idx < partitions.size(); ++part_idx) {
        std::cout << "  Partition " << part_idx << ":" << std::endl;
        const Partition &partition = partitions[part_idx];
        const std::vector<NumericRange> &ranges = partition.get_ranges();
        for (const auto &range : ranges) {
            // Print opening bracket/parenthesis
            std::cout << "    " << (range.lower_inclusive ? "[" : "(");
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
            // Print closing bracket/parenthesis
            std::cout << (range.upper_inclusive ? "]" : ")") << std::endl;
        }
    }
}

std::pair<ap_float, ap_float> NumericDomainMapping::get_partition_bounding_box(int partition_index) const {
    if (partition_index < 0 || partition_index >= static_cast<int>(partitions.size())) {
        return {-std::numeric_limits<ap_float>::infinity(),
                std::numeric_limits<ap_float>::infinity()};
    }
    
    return partitions[partition_index].get_bounding_box();
}

int NumericDomainMapping::evaluate_comparison_with(
    const NumericDomainMapping &other,
    int my_partition,
    int other_partition,
    comp_operator op) const {
    
    // Check partition indices are valid
    if (my_partition < 0 || my_partition >= static_cast<int>(partitions.size()) ||
        other_partition < 0 || other_partition >= static_cast<int>(other.partitions.size())) {
        return 2; // unknown if partition not found
    }
    
    // Use the Partition class's evaluate_comparison method
    return partitions[my_partition].evaluate_comparison(other.partitions[other_partition], op);
}

std::vector<int> NumericDomainMapping::compute_reachable_partitions(
    int source_partition,
    f_operator effect_op,
    ap_float operand_value) const {
    
    std::vector<int> reachable_partitions;
    
    // Check partition index is valid
    if (source_partition < 0 || source_partition >= static_cast<int>(partitions.size())) {
        // Source partition not found - return empty vector
        return reachable_partitions;
    }
    
    // Apply the effect to the source partition to get the result partition
    const Partition &source_part = partitions[source_partition];
    Partition result_partition = source_part.apply_operation(effect_op, operand_value);
    
    // Find all partitions that overlap with the result partition
    for (size_t i = 0; i < partitions.size(); ++i) {
        // Check if partitions[i] intersects with result_partition
        Partition intersection = partitions[i].intersect_with(result_partition);
        if (!intersection.is_empty()) {
            reachable_partitions.push_back(static_cast<int>(i));
        }
    }
    
    // If no partitions found (shouldn't happen with proper partitioning),
    // return all partitions conservatively
    if (reachable_partitions.empty()) {
        for (size_t i = 0; i < partitions.size(); ++i) {
            reachable_partitions.push_back(static_cast<int>(i));
        }
    }
    
    return reachable_partitions;
}

std::vector<int> NumericDomainMapping::apply_effect_to_partition(
    int source_partition_index, f_operator op, ap_float operand) const {
    
    // This is just an alias for compute_reachable_partitions
    return compute_reachable_partitions(source_partition_index, op, operand);
}

std::pair<ap_float, ap_float> NumericDomainMapping::apply_range_operation(
    ap_float left_lower, ap_float left_upper,
    ap_float right_lower, ap_float right_upper,
    cal_operator op) {
    
    ap_float result_lower, result_upper;
    
    switch (op) {
        case sum: // left + right
            result_lower = left_lower + right_lower;
            result_upper = left_upper + right_upper;
            break;
            
        case diff: // left - right
            // [a,b) - [c,d) = [a-d, b-c)
            result_lower = left_lower - right_upper;
            result_upper = left_upper - right_lower;
            break;
            
        case mult: // left * right
            // Need to consider all four combinations and take min/max
            {
                ap_float products[4] = {
                    left_lower * right_lower,
                    left_lower * right_upper,
                    left_upper * right_lower,
                    left_upper * right_upper
                };
                result_lower = *std::min_element(products, products + 4);
                result_upper = *std::max_element(products, products + 4);
            }
            break;
            
        case divi: // left / right
            // Need to check for division by zero
            {
                // If right range contains zero, result is undefined
                if (right_lower < 0 && right_upper > 0) {
                    // Range spans zero - return infinite range
                    result_lower = -std::numeric_limits<ap_float>::infinity();
                    result_upper = std::numeric_limits<ap_float>::infinity();
                } else if (right_lower == 0 && right_upper == 0) {
                    // Division by zero - undefined
                    result_lower = -std::numeric_limits<ap_float>::infinity();
                    result_upper = std::numeric_limits<ap_float>::infinity();
                } else {
                    // Safe to divide - check all four combinations
                    ap_float quotients[4] = {
                        left_lower / right_lower,
                        left_lower / right_upper,
                        left_upper / right_lower,
                        left_upper / right_upper
                    };
                    result_lower = *std::min_element(quotients, quotients + 4);
                    result_upper = *std::max_element(quotients, quotients + 4);
                }
            }
            break;
            
        default:
            // Unknown operator, return infinite range
            result_lower = -std::numeric_limits<ap_float>::infinity();
            result_upper = std::numeric_limits<ap_float>::infinity();
            break;
    }
    
    return std::make_pair(result_lower, result_upper);
}

// ============================================================================
// Partition class implementation
// ============================================================================

bool NumericRange::overlaps_with(const NumericRange &other) const {
    return overlaps_with(other.lower, other.upper, other.lower_inclusive, other.upper_inclusive);
}

NumericRange NumericRange::intersect(const NumericRange &other) const {
    // Compute intersection of two ranges
    ap_float new_lower = std::max(lower, other.lower);
    ap_float new_upper = std::min(upper, other.upper);
    
    // Determine inclusiveness of boundaries
    bool new_lower_inclusive;
    if (lower == other.lower) {
        new_lower_inclusive = lower_inclusive && other.lower_inclusive;
    } else if (lower > other.lower) {
        new_lower_inclusive = lower_inclusive;
    } else {
        new_lower_inclusive = other.lower_inclusive;
    }
    
    bool new_upper_inclusive;
    if (upper == other.upper) {
        new_upper_inclusive = upper_inclusive && other.upper_inclusive;
    } else if (upper < other.upper) {
        new_upper_inclusive = upper_inclusive;
    } else {
        new_upper_inclusive = other.upper_inclusive;
    }
    
    // Check if result is empty
    if (new_lower > new_upper) {
        // Empty range - return an explicitly empty range
        return NumericRange(0, 0, false, false);
    }
    if (new_lower == new_upper && (!new_lower_inclusive || !new_upper_inclusive)) {
        // Single point but not both inclusive - empty
        return NumericRange(0, 0, false, false);
    }
    
    return NumericRange(new_lower, new_upper, new_lower_inclusive, new_upper_inclusive);
}

Partition::Partition(const std::vector<NumericRange> &input_ranges) {
    // Sort ranges by lower bound and merge overlapping/adjacent ranges
    std::vector<NumericRange> sorted = input_ranges;
    std::sort(sorted.begin(), sorted.end(), 
              [](const NumericRange &a, const NumericRange &b) {
                  return a.lower < b.lower || (a.lower == b.lower && a.lower_inclusive > b.lower_inclusive);
              });
    
    // Add non-empty ranges
    for (const auto &range : sorted) {
        if (!range.is_empty()) {
            add_range(range);
        }
    }
}

void Partition::add_range(const NumericRange &range) {
    if (range.is_empty()) return;
    
    // Find insertion position (maintain sorted order)
    auto it = std::lower_bound(ranges.begin(), ranges.end(), range,
                               [](const NumericRange &a, const NumericRange &b) {
                                   return a.lower < b.lower || 
                                          (a.lower == b.lower && a.lower_inclusive > b.lower_inclusive);
                               });
    
    ranges.insert(it, range);
    
    // TODO: Could merge overlapping/adjacent ranges here for optimization
}

std::pair<ap_float, ap_float> Partition::get_bounding_box() const {
    if (ranges.empty()) {
        return {0, 0};  // Empty partition
    }
    
    ap_float min_lower = ranges.front().lower;
    ap_float max_upper = ranges.back().upper;
    
    return {min_lower, max_upper};
}

bool Partition::is_full_range() const {
    // Check if partition covers entire real line
    // This requires checking if union of all ranges = (-inf, +inf)
    if (ranges.empty()) return false;
    
    // Simple check: if there's a single range covering everything
    if (ranges.size() == 1 && ranges[0].is_full_range()) {
        return true;
    }
    
    // More complex: check if ranges cover everything without gaps
    // For now, conservative approach
    return false;
}

Partition Partition::union_with(const Partition &other) const {
    std::vector<NumericRange> combined = ranges;
    combined.insert(combined.end(), other.ranges.begin(), other.ranges.end());
    return Partition(combined);
}

Partition Partition::intersect_with(const Partition &other) const {
    std::vector<NumericRange> result_ranges;
    
    // Compute intersection: for each range in this, intersect with each range in other
    for (const auto &r1 : ranges) {
        for (const auto &r2 : other.ranges) {
            if (r1.overlaps_with(r2)) {
                NumericRange intersection = r1.intersect(r2);
                if (!intersection.is_empty()) {
                    result_ranges.push_back(intersection);
                }
            }
        }
    }
    
    return Partition(result_ranges);
}

Partition Partition::complement() const {
    if (ranges.empty()) {
        // Empty partition - complement is full range
        return Partition(NumericRange(-std::numeric_limits<ap_float>::infinity(),
                                     std::numeric_limits<ap_float>::infinity(),
                                     true, false));
    }
    
    std::vector<NumericRange> complement_ranges;
    
    // Add range before first range if it doesn't start at -inf
    if (ranges.front().lower != -std::numeric_limits<ap_float>::infinity()) {
        complement_ranges.emplace_back(
            -std::numeric_limits<ap_float>::infinity(),
            ranges.front().lower,
            true,
            !ranges.front().lower_inclusive);
    }
    
    // Add gaps between consecutive ranges
    for (size_t i = 0; i + 1 < ranges.size(); ++i) {
        ap_float gap_lower = ranges[i].upper;
        ap_float gap_upper = ranges[i+1].lower;
        bool gap_lower_inclusive = !ranges[i].upper_inclusive;
        bool gap_upper_inclusive = !ranges[i+1].lower_inclusive;
        
        if (gap_lower < gap_upper || (gap_lower == gap_upper && gap_lower_inclusive && gap_upper_inclusive)) {
            complement_ranges.emplace_back(gap_lower, gap_upper, 
                                          gap_lower_inclusive, gap_upper_inclusive);
        }
    }
    
    // Add range after last range if it doesn't end at +inf
    if (ranges.back().upper != std::numeric_limits<ap_float>::infinity()) {
        complement_ranges.emplace_back(
            ranges.back().upper,
            std::numeric_limits<ap_float>::infinity(),
            !ranges.back().upper_inclusive,
            false);
    }
    
    return Partition(complement_ranges);
}

Partition Partition::apply_operation(f_operator op, ap_float operand) const {
    std::vector<NumericRange> result_ranges;
    
    for (const auto &range : ranges) {
        ap_float new_lower, new_upper;
        bool new_lower_inclusive = range.lower_inclusive;
        bool new_upper_inclusive = range.upper_inclusive;
        
        switch (op) {
            case f_operator::assign:
                // Assignment: entire partition becomes single value
                new_lower = new_upper = operand;
                new_lower_inclusive = new_upper_inclusive = true;
                break;
                
            case f_operator::increase:
                // Add constant to range
                new_lower = range.lower + operand;
                new_upper = range.upper + operand;
                break;
                
            case f_operator::decrease:
                // Subtract constant from range
                new_lower = range.lower - operand;
                new_upper = range.upper - operand;
                break;
                
            case f_operator::scale_up:
                // Multiply by constant (positive)
                if (operand >= 0) {
                    new_lower = range.lower * operand;
                    new_upper = range.upper * operand;
                } else {
                    // Negative multiplier flips the range
                    new_lower = range.upper * operand;
                    new_upper = range.lower * operand;
                    std::swap(new_lower_inclusive, new_upper_inclusive);
                }
                break;
                
            case f_operator::scale_down:
                // Divide by constant (avoid division by zero)
                if (operand != 0) {
                    if (operand > 0) {
                        new_lower = range.lower / operand;
                        new_upper = range.upper / operand;
                    } else {
                        // Negative divisor flips the range
                        new_lower = range.upper / operand;
                        new_upper = range.lower / operand;
                        std::swap(new_lower_inclusive, new_upper_inclusive);
                    }
                } else {
                    // Division by zero - undefined, return full range
                    new_lower = -std::numeric_limits<ap_float>::infinity();
                    new_upper = std::numeric_limits<ap_float>::infinity();
                }
                break;
                
            default:
                // Unknown operation
                new_lower = -std::numeric_limits<ap_float>::infinity();
                new_upper = std::numeric_limits<ap_float>::infinity();
                break;
        }
        
        result_ranges.emplace_back(new_lower, new_upper, 
                                  new_lower_inclusive, new_upper_inclusive);
    }
    
    return Partition(result_ranges);
}

Partition Partition::apply_binary_operation(
    const Partition &left, const Partition &right, cal_operator op) {
    
    std::vector<NumericRange> result_ranges;
    
    // Apply operation to all combinations of ranges from left and right
    for (const auto &l : left.ranges) {
        for (const auto &r : right.ranges) {
            auto result = NumericDomainMapping::apply_range_operation(
                l.lower, l.upper, r.lower, r.upper, op);
            
            // For now, use conservative closed interval
            result_ranges.emplace_back(result.first, result.second, true, true);
        }
    }
    
    return Partition(result_ranges);
}

// Static helper for evaluating comparison between two ranges
int Partition::evaluate_range_comparison(
    comp_operator op,
    ap_float left_lower, ap_float left_upper,
    ap_float right_lower, ap_float right_upper) {
    
    // Returns: 0=TRUE, 1=FALSE, 2=UNKNOWN
    switch (op) {
        case comp_operator::lt: // left < right
            if (left_upper <= right_lower) {
                return 0; // TRUE
            } else if (left_lower >= right_upper) {
                return 1; // FALSE
            } else {
                return 2; // UNKNOWN
            }
            
        case comp_operator::le: // left <= right
            if (left_upper <= right_lower) {
                return 0; // TRUE
            } else if (left_lower > right_upper) {
                return 1; // FALSE
            } else {
                return 2; // UNKNOWN
            }
            
        case comp_operator::eq: // left == right
            if (left_lower == left_upper && right_lower == right_upper && 
                left_lower == right_lower) {
                return 0; // TRUE (both are same point)
            }
            else if (left_upper <= right_lower || right_upper <= left_lower) {
                return 1; // FALSE (no overlap)
            } else {
                return 2; // UNKNOWN
            }
            
        case comp_operator::ge: // left >= right
            if (left_lower >= right_upper) {
                return 0; // TRUE
            } else if (left_upper < right_lower) {
                return 1; // FALSE
            } else {
                return 2; // UNKNOWN
            }
            
        case comp_operator::gt: // left > right
            if (left_lower >= right_upper) {
                return 0; // TRUE
            } else if (left_upper <= right_lower) {
                return 1; // FALSE
            } else {
                return 2; // UNKNOWN
            }
            
        default:
            return 2; // UNKNOWN for unrecognized operators
    }
}

int Partition::evaluate_comparison(const Partition &other, comp_operator op) const {
    // Evaluate comparison between two partitions
    // Returns: 0=TRUE, 1=FALSE, 2=UNKNOWN
    
    if (ranges.empty() || other.ranges.empty()) {
        return 2;  // UNKNOWN for empty partitions
    }
    
    // For each combination of ranges, evaluate comparison
    // If all combinations give same result, return that result
    // Otherwise return UNKNOWN
    
    int first_result = -1;
    for (const auto &l : ranges) {
        for (const auto &r : other.ranges) {
            int result = Partition::evaluate_range_comparison(
                op, l.lower, l.upper, r.lower, r.upper);
            
            if (first_result == -1) {
                first_result = result;
            } else if (first_result != result) {
                // Different results for different range combinations
                return 2;  // UNKNOWN
            }
        }
    }
    
    return (first_result == -1) ? 2 : first_result;
}

void Partition::dump(std::ostream &out) const {
    out << "Partition with " << ranges.size() << " range(s): ";
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0) out << " ∪ ";
        out << (ranges[i].lower_inclusive ? "[" : "(");
        if (ranges[i].lower == -std::numeric_limits<ap_float>::infinity()) {
            out << "-∞";
        } else {
            out << ranges[i].lower;
        }
        out << ", ";
        if (ranges[i].upper == std::numeric_limits<ap_float>::infinity()) {
            out << "∞";
        } else {
            out << ranges[i].upper;
        }
        out << (ranges[i].upper_inclusive ? "]" : ")");
    }
}

bool Partition::is_valid() const {
    // Check ranges are sorted and non-overlapping
    for (size_t i = 0; i + 1 < ranges.size(); ++i) {
        if (ranges[i].is_empty()) return false;
        
        // Check sorted
        if (ranges[i].lower > ranges[i+1].lower) return false;
        
        // Check non-overlapping
        if (ranges[i].upper > ranges[i+1].lower) return false;
        if (ranges[i].upper == ranges[i+1].lower) {
            // Adjacent ranges - both can't include the boundary
            if (ranges[i].upper_inclusive && ranges[i+1].lower_inclusive) {
                return false;  // Overlap at boundary
            }
        }
    }
    
    return true;
}

}

