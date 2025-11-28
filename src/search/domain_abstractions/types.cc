#include "types.h"

#include "../utils/hash.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>

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
    bool old_lower_inclusive = old_range.lower_inclusive;
    bool old_upper_inclusive = old_range.upper_inclusive;
    
    // Replace the old range with the lower part [old_lower, n), keeping old partition index
    // Lower boundary comes from original range, upper boundary is exclusive at n
    ranges[range_index] = NumericRange(old_lower, n, old_lower_inclusive, false, old_partition);
    
    // Insert the upper part [n, old_upper) with new partition index
    // Lower boundary is inclusive at n, upper boundary comes from original range
    ranges.insert(ranges.begin() + range_index + 1,
                  NumericRange(n, old_upper, true, old_upper_inclusive, new_partition));
    
    return get_num_partitions();
}

// ExclusionSplitMapping implementation
int ExclusionSplitMapping::split_at(ap_float n) {
    // Find the range containing n
    int range_index = -1;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].contains(n)) {
            range_index = i;
            break;
        }
    }
    
    // If n is not in any range, do nothing
    if (range_index == -1) {
        return get_num_partitions();
    }
    
    NumericRange &old_range = ranges[range_index];
    
    // If n is already at a boundary, we need different logic
    // For simplicity, assume n is strictly inside the range
    if (old_range.lower == n || old_range.upper == n) {
        // Could handle this, but for now just don't split
        return get_num_partitions();
    }
    
    int num_partitions = get_num_partitions();
    int old_partition = old_range.partition_index;   // Reuse for one side (e.g., lower)
    int point_partition = num_partitions;            // New partition for {x}
    int upper_partition = num_partitions + 1;        // New partition for (x, old_upper)
    
    ap_float old_lower = old_range.lower;
    ap_float old_upper = old_range.upper;
    bool old_lower_inclusive = old_range.lower_inclusive;
    bool old_upper_inclusive = old_range.upper_inclusive;
    
    // Create three ranges:
    // 1. Lower part: (old_lower, n) with old partition
    //    Upper boundary is exclusive at n
    ranges[range_index] = NumericRange(old_lower, n, old_lower_inclusive, false, old_partition);
    
    // 2. Point: [n, n] with new partition ({x})
    //    Both boundaries inclusive for single point
    ranges.insert(ranges.begin() + range_index + 1,
                  NumericRange(n, n, true, true, point_partition));
    
    // 3. Upper part: (n, old_upper) with a NEW partition (no longer share old partition)
    //    Lower boundary is exclusive at n
    ranges.insert(ranges.begin() + range_index + 2,
                  NumericRange(n, old_upper, false, old_upper_inclusive, upper_partition));
    
    return get_num_partitions();
}

void NumericDomainMapping::dump() const {
    std::cout << "NumericDomainMapping with " << get_num_partitions() 
              << " partitions and " << get_num_ranges() << " ranges:" << std::endl;
    for (size_t i = 0; i < ranges.size(); ++i) {
        const auto &range = ranges[i];
        // Print opening bracket/parenthesis
        std::cout << "  Range " << i << ": " << (range.lower_inclusive ? "[" : "(");
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
        std::cout << (range.upper_inclusive ? "]" : ")");
        std::cout << " -> partition " << range.partition_index << std::endl;
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
    return evaluate_comparison(op, *my_range, *other_range);
}

std::vector<int> NumericDomainMapping::compute_reachable_partitions(
    int source_partition,
    f_operator effect_op,
    ap_float operand_value) const {
    
    std::vector<int> reachable_partitions;
    
    // Get the source partition range
    const NumericRange *source_range = get_range_for_partition(source_partition);
    if (!source_range) {
        // Source partition not found - return empty vector
        return reachable_partitions;
    }
    
    // Apply the effect to compute the result range using forward/progression semantics
    // This computes where the variable can end up after applying the effect
    ap_float result_lower, result_upper;
    ap_float source_lower = source_range->lower;
    ap_float source_upper = source_range->upper;
    bool source_lower_inclusive = source_range->lower_inclusive;
    bool source_upper_inclusive = source_range->upper_inclusive;
    
    // Track inclusivity for the computed result interval. These should be derived
    // from the source interval and the operator semantics.
    bool result_lower_inclusive = false;
    bool result_upper_inclusive = false;

    switch (effect_op) {
        case assign:
            // x := c  -->  result is [c, c]
            result_lower = operand_value;
            result_upper = operand_value;
            result_lower_inclusive = true;
            result_upper_inclusive = true;
            break;
            
        case increase:
            // x += c  -->  [lower + c, upper + c] (preserve inclusivity)
            result_lower = source_lower + operand_value;
            result_upper = source_upper + operand_value;
            result_lower_inclusive = source_lower_inclusive;
            result_upper_inclusive = source_upper_inclusive;
            break;
            
        case decrease:
            // x -= c  -->  [lower - c, upper - c] (preserve inclusivity)
            result_lower = source_lower - operand_value;
            result_upper = source_upper - operand_value;
            result_lower_inclusive = source_lower_inclusive;
            result_upper_inclusive = source_upper_inclusive;
            break;
            
        case scale_up:
            // x *= c  -->  depends on sign of c and bounds
            if (operand_value > 0) {
                result_lower = source_lower * operand_value;
                result_upper = source_upper * operand_value;
                // Positive scaling preserves order and inclusivity
                result_lower_inclusive = source_lower_inclusive;
                result_upper_inclusive = source_upper_inclusive;
            } else if (operand_value < 0) {
                // Negative multiplier flips the order
                result_lower = source_upper * operand_value;
                result_upper = source_lower * operand_value;
                // When order flips, the boundary inclusivity swaps as well
                result_lower_inclusive = source_upper_inclusive;
                result_upper_inclusive = source_lower_inclusive;
            } else {
                // Multiply by zero
                result_lower = 0;
                result_upper = 0;
                result_lower_inclusive = true;
                result_upper_inclusive = true;
            }
            break;
            
        case scale_down:
            // x /= c  -->  similar to scale_up but with division
            if (operand_value > 0) {
                result_lower = source_lower / operand_value;
                result_upper = source_upper / operand_value;
                // Positive division preserves order and inclusivity
                result_lower_inclusive = source_lower_inclusive;
                result_upper_inclusive = source_upper_inclusive;
            } else if (operand_value < 0) {
                // Negative divisor flips the order
                result_lower = source_upper / operand_value;
                result_upper = source_lower / operand_value;
                // When order flips, the boundary inclusivity swaps as well
                result_lower_inclusive = source_upper_inclusive;
                result_upper_inclusive = source_lower_inclusive;
            } else {
                // Division by zero - undefined, return all partitions
                for (const auto &range : ranges) {
                    reachable_partitions.push_back(range.partition_index);
                }
                return reachable_partitions;
            }
            break;
            
        default:
            // Unknown operator, return all partitions conservatively
            for (const auto &range : ranges) {
                reachable_partitions.push_back(range.partition_index);
            }
            return reachable_partitions;
    }
    
    // Find all partitions that overlap with the result range
    for (const auto &range : ranges) {
        // Use the overlaps_with method which handles boundary inclusivity
        bool overlaps = range.overlaps_with(result_lower, result_upper, 
                                           result_lower_inclusive, result_upper_inclusive);
        
        if (overlaps) {
            reachable_partitions.push_back(range.partition_index);
        }
    }
    
    // If no partitions found (shouldn't happen with proper partitioning),
    // return all partitions conservatively
    if (reachable_partitions.empty()) {
        for (const auto &range : ranges) {
            reachable_partitions.push_back(range.partition_index);
        }
    }
    
    return reachable_partitions;
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
            // Handle multiplication by zero explicitly to avoid NaN with infinity
            if ((left_lower == 0 && left_upper == 0) || (right_lower == 0 && right_upper == 0)) {
                result_lower = 0;
                result_upper = 0;
                break;
            }

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

NumericRange NumericDomainMapping::apply_range_operation(
    const NumericRange &left, const NumericRange &right, cal_operator op) {
    
    ap_float result_lower, result_upper;
    bool result_lower_inclusive = false;
    bool result_upper_inclusive = false;
    
    switch (op) {
        case sum: // left + right
            result_lower = left.lower + right.lower;
            result_upper = left.upper + right.upper;
            result_lower_inclusive = left.lower_inclusive && right.lower_inclusive;
            result_upper_inclusive = left.upper_inclusive && right.upper_inclusive;
            break;
            
        case diff: // left - right
            // [a,b) - [c,d) = [a-d, b-c)
            result_lower = left.lower - right.upper;
            result_upper = left.upper - right.lower;
            result_lower_inclusive = left.lower_inclusive && right.upper_inclusive;
            result_upper_inclusive = left.upper_inclusive && right.lower_inclusive;
            break;
            
        case mult: // left * right
            // Handle multiplication by zero explicitly to avoid NaN with infinity
            if ((left.lower == 0 && left.upper == 0) || (right.lower == 0 && right.upper == 0)) {
                result_lower = 0;
                result_upper = 0;
                result_lower_inclusive = true;
                result_upper_inclusive = true;
                break;
            }

            // Need to consider all four combinations and take min/max
            {
                ap_float products[4] = {
                    left.lower * right.lower,
                    left.lower * right.upper,
                    left.upper * right.lower,
                    left.upper * right.upper
                };
                
                // Determine which product is min/max
                int min_idx = 0;
                int max_idx = 0;
                for (int i = 1; i < 4; ++i) {
                    if (products[i] < products[min_idx]) min_idx = i;
                    if (products[i] > products[max_idx]) max_idx = i;
                }
                
                result_lower = products[min_idx];
                result_upper = products[max_idx];
                
                // Determine inclusivity based on the operands that produced min/max
                // 0: ll * rl -> l_inc && r_inc
                // 1: ll * ru -> l_inc && r_inc (upper of right is effectively lower of negated right?)
                // Actually, just check the source boundaries.
                
                auto get_inclusivity = [&](int idx) {
                    switch(idx) {
                        case 0: return left.lower_inclusive && right.lower_inclusive;
                        case 1: return left.lower_inclusive && right.upper_inclusive;
                        case 2: return left.upper_inclusive && right.lower_inclusive;
                        case 3: return left.upper_inclusive && right.upper_inclusive;
                        default: return false;
                    }
                };
                
                result_lower_inclusive = get_inclusivity(min_idx);
                result_upper_inclusive = get_inclusivity(max_idx);
                
                // If multiple products are equal to min/max, we need to be careful.
                // The result is inclusive if ANY combination producing the extremum is inclusive?
                // No, the result is the union of possible values.
                // If we have [0, 1] * [0, 1], min is 0. 0*0=0 (inc), 0*1=0 (inc).
                // If we have (0, 1) * (0, 1), min is 0. 0*0=0 (exc).
                // If we have [0, 1] * (0, 1), min is 0. 0*0=0 (exc).
                // So if ANY combination producing the min is exclusive, the result is exclusive?
                // Wait. The range of products is continuous.
                // The lower bound is inclusive if there exist x in left, y in right such that x*y = result_lower.
                // This happens if the combination producing result_lower uses inclusive bounds.
                // If there are multiple combinations producing the same min, and ONE of them is inclusive, then the value is achievable.
                // So we should OR the inclusivity for ties.
                
                for (int i = 0; i < 4; ++i) {
                    if (products[i] == result_lower) {
                        result_lower_inclusive = result_lower_inclusive || get_inclusivity(i);
                    }
                    if (products[i] == result_upper) {
                        result_upper_inclusive = result_upper_inclusive || get_inclusivity(i);
                    }
                }
            }
            break;
            
        case divi: // left / right
            // Need to check for division by zero
            {
                // If right range contains zero, result is undefined
                if (right.lower < 0 && right.upper > 0) {
                    // Range spans zero - return infinite range
                    result_lower = -std::numeric_limits<ap_float>::infinity();
                    result_upper = std::numeric_limits<ap_float>::infinity();
                    result_lower_inclusive = false;
                    result_upper_inclusive = false;
                } else if (right.lower == 0 && right.upper == 0) {
                    // Division by zero - undefined
                    result_lower = -std::numeric_limits<ap_float>::infinity();
                    result_upper = std::numeric_limits<ap_float>::infinity();
                    result_lower_inclusive = false;
                    result_upper_inclusive = false;
                } else {
                    // Safe to divide - check all four combinations
                    ap_float quotients[4] = {
                        left.lower / right.lower,
                        left.lower / right.upper,
                        left.upper / right.lower,
                        left.upper / right.upper
                    };
                    
                    int min_idx = 0;
                    int max_idx = 0;
                    for (int i = 1; i < 4; ++i) {
                        if (quotients[i] < quotients[min_idx]) min_idx = i;
                        if (quotients[i] > quotients[max_idx]) max_idx = i;
                    }
                    
                    result_lower = quotients[min_idx];
                    result_upper = quotients[max_idx];
                    
                    auto get_inclusivity = [&](int idx) {
                        switch(idx) {
                            case 0: return left.lower_inclusive && right.lower_inclusive;
                            case 1: return left.lower_inclusive && right.upper_inclusive;
                            case 2: return left.upper_inclusive && right.lower_inclusive;
                            case 3: return left.upper_inclusive && right.upper_inclusive;
                            default: return false;
                        }
                    };
                    
                    result_lower_inclusive = get_inclusivity(min_idx);
                    result_upper_inclusive = get_inclusivity(max_idx);
                    
                    for (int i = 0; i < 4; ++i) {
                        if (quotients[i] == result_lower) {
                            result_lower_inclusive = result_lower_inclusive || get_inclusivity(i);
                        }
                        if (quotients[i] == result_upper) {
                            result_upper_inclusive = result_upper_inclusive || get_inclusivity(i);
                        }
                    }
                }
            }
            break;
            
        default:
            // Unknown operator, return infinite range
            result_lower = -std::numeric_limits<ap_float>::infinity();
            result_upper = std::numeric_limits<ap_float>::infinity();
            break;
    }
    
    return NumericRange(result_lower, result_upper, result_lower_inclusive, result_upper_inclusive);
}

int NumericDomainMapping::evaluate_comparison(
    comp_operator op,
    const NumericRange &left, const NumericRange &right) {
    
    // Evaluate based on comparison operator
    // Returns: 0 = definitely false, 1 = definitely true, 2 = unknown
    
    switch (op) {
        case comp_operator::lt: // left < right
            // Definitely true if: max(left) < min(right)
            // OR max(left) == min(right) AND (max(left) exclusive OR min(right) exclusive)
            if (left.upper < right.lower) {
                return 0; // definitely true
            } else if (left.upper == right.lower && (!left.upper_inclusive || !right.lower_inclusive)) {
                return 0; // definitely true
            }
            
            // Definitely false if: min(left) >= max(right)
            // OR min(left) == max(right) AND (min(left) inclusive AND max(right) inclusive) -> actually if they are equal, it is NOT <.
            // So false if min(left) > max(right)
            // OR min(left) == max(right) (always false for <)
            if (left.lower >= right.upper) {
                return 1; // definitely false
            }
            return 2; // unknown
            
        case comp_operator::le: // left <= right
            // Definitely true if: max(left) <= min(right)
            // Wait, if max(left) == min(right), we need both inclusive to be sure?
            // No, if max(left) == min(right), then any x in left is <= max(left) == min(right) <= any y in right.
            // So x <= y is always true.
            if (left.upper <= right.lower) {
                return 0; // definitely true
            }
            
            // Definitely false if: min(left) > max(right)
            // OR min(left) == max(right) AND (min(left) exclusive OR max(right) exclusive)
            if (left.lower > right.upper) {
                return 1; // definitely false
            } else if (left.lower == right.upper && (!left.lower_inclusive || !right.upper_inclusive)) {
                return 1; // definitely false
            }
            return 2; // unknown
            
        case comp_operator::eq: // left == right
            // Definitely true only if both ranges are the same single point
            if (left.lower == left.upper && right.lower == right.upper && 
                left.lower == right.lower &&
                left.lower_inclusive && left.upper_inclusive &&
                right.lower_inclusive && right.upper_inclusive) {
                return 0; // definitely true
            }
            // Definitely false if ranges don't overlap
            if (!left.overlaps_with(right)) {
                return 1; // definitely false
            }
            return 2; // unknown
            
        case comp_operator::ge: // left >= right
            // Definitely true if: min(left) >= max(right)
            if (left.lower >= right.upper) {
                return 0; // definitely true
            }
            
            // Definitely false if: max(left) < min(right)
            // OR max(left) == min(right) AND (max(left) exclusive OR min(right) exclusive)
            if (left.upper < right.lower) {
                return 1; // definitely false
            } else if (left.upper == right.lower && (!left.upper_inclusive || !right.lower_inclusive)) {
                return 1; // definitely false
            }
            return 2; // unknown
            
        case comp_operator::gt: // left > right
            // Definitely true if: min(left) > max(right)
            // OR min(left) == max(right) AND (min(left) exclusive OR max(right) exclusive)
            if (left.lower > right.upper) {
                return 0; // definitely true
            } else if (left.lower == right.upper && (!left.lower_inclusive || !right.upper_inclusive)) {
                return 0; // definitely true
            }
            
            // Definitely false if: max(left) <= min(right)
            if (left.upper <= right.lower) {
                return 1; // definitely false
            }
            return 2; // unknown
            
        default:
            return 2; // unknown
    }
}

// ============================================================================
// Partition class implementation
// ============================================================================
// An idea I had before when partitions can consist of multiple intervals. Deprecated for now.

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
        return NumericRange(0, 0, false, false, -1);
    }
    if (new_lower == new_upper && (!new_lower_inclusive || !new_upper_inclusive)) {
        // Single point but not both inclusive - empty
        return NumericRange(0, 0, false, false, -1);
    }
    
    return NumericRange(new_lower, new_upper, new_lower_inclusive, new_upper_inclusive, partition_index);
}

std::string NumericRange::to_string() const {
    std::ostringstream oss;
    oss << (lower_inclusive ? "[" : "(");
    if (lower == -std::numeric_limits<ap_float>::infinity()) {
        oss << "-inf";
    } else {
        oss << lower;
    }
    oss << ", ";
    if (upper == std::numeric_limits<ap_float>::infinity()) {
        oss << "inf";
    } else {
        oss << upper;
    }
    oss << (upper_inclusive ? "]" : ")");
    return oss.str();
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
                                     true, false, 0));
    }
    
    std::vector<NumericRange> complement_ranges;
    
    // Add range before first range if it doesn't start at -inf
    if (ranges.front().lower != -std::numeric_limits<ap_float>::infinity()) {
        complement_ranges.emplace_back(
            -std::numeric_limits<ap_float>::infinity(),
            ranges.front().lower,
            true,
            !ranges.front().lower_inclusive,
            0);
    }
    
    // Add gaps between consecutive ranges
    for (size_t i = 0; i + 1 < ranges.size(); ++i) {
        ap_float gap_lower = ranges[i].upper;
        ap_float gap_upper = ranges[i+1].lower;
        bool gap_lower_inclusive = !ranges[i].upper_inclusive;
        bool gap_upper_inclusive = !ranges[i+1].lower_inclusive;
        
        if (gap_lower < gap_upper || (gap_lower == gap_upper && gap_lower_inclusive && gap_upper_inclusive)) {
            complement_ranges.emplace_back(gap_lower, gap_upper, 
                                          gap_lower_inclusive, gap_upper_inclusive, 0);
        }
    }
    
    // Add range after last range if it doesn't end at +inf
    if (ranges.back().upper != std::numeric_limits<ap_float>::infinity()) {
        complement_ranges.emplace_back(
            ranges.back().upper,
            std::numeric_limits<ap_float>::infinity(),
            !ranges.back().upper_inclusive,
            false,
            0);
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
                                  new_lower_inclusive, new_upper_inclusive, 0);
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
            result_ranges.emplace_back(result.first, result.second, true, true, 0);
        }
    }
    
    return Partition(result_ranges);
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
            int result = NumericDomainMapping::evaluate_comparison(op, l, r);
            
            if (first_result == -1) {
                first_result = result;
            } else if (first_result != result) {
                // Different results for different range combinations
                return 2;  // UNKNOWN
            }
        }
    }
    
    return first_result;
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

// ============================================================================
// NumericDomainMapping - Partition Integration
// ============================================================================

Partition NumericDomainMapping::get_partition(int partition_index) const {
    std::vector<NumericRange> partition_ranges;
    
    // Collect all ranges that belong to this partition
    for (const auto &range : ranges) {
        if (range.partition_index == partition_index) {
            partition_ranges.push_back(range);
        }
    }
    
    if (partition_ranges.empty()) {
        // No ranges found for this partition - return empty partition
        return Partition();
    }
    
    return Partition(partition_ranges);
}

std::pair<ap_float, ap_float> NumericDomainMapping::get_partition_bounding_box(int partition_index) const {
    Partition p = get_partition(partition_index);
    return p.get_bounding_box();
}

std::vector<int> NumericDomainMapping::get_all_partition_indices() const {
    std::vector<int> indices;
    
    // Collect unique partition indices
    for (const auto &range : ranges) {
        if (std::find(indices.begin(), indices.end(), range.partition_index) == indices.end()) {
            indices.push_back(range.partition_index);
        }
    }
    
    // Sort for consistency
    std::sort(indices.begin(), indices.end());
    
    return indices;
}

int NumericDomainMapping::evaluate_partition_comparison(
    const Partition &left, const Partition &right, comp_operator op) {
    return left.evaluate_comparison(right, op);
}

std::vector<int> NumericDomainMapping::apply_effect_to_partition(
    int source_partition_index, f_operator op, ap_float operand) const {
    
    // Get the source partition
    Partition source = get_partition(source_partition_index);
    
    if (source.is_empty()) {
        return {};  // Empty source -> no results
    }
    
    // Apply the operation to get result partition
    Partition result = source.apply_operation(op, operand);
    
    // Determine which partition indices the result could map to
    std::vector<int> reachable_partitions;
    
    // For each range in the result, check which partitions it overlaps with
    for (const auto &result_range : result.get_ranges()) {
        // Sample points in the result range to determine partition membership
        // Use lower bound, upper bound, and midpoint
        std::vector<ap_float> sample_points;
        
        if (result_range.lower != -std::numeric_limits<ap_float>::infinity()) {
            ap_float sample = result_range.lower;
            if (!result_range.lower_inclusive && result_range.lower < result_range.upper) {
                // If lower is exclusive, sample slightly above it
                sample += 0.0001;  // Small epsilon
            }
            sample_points.push_back(sample);
        }
        
        if (result_range.upper != std::numeric_limits<ap_float>::infinity()) {
            ap_float sample = result_range.upper;
            if (!result_range.upper_inclusive && result_range.lower < result_range.upper) {
                // If upper is exclusive, sample slightly below it
                sample -= 0.0001;  // Small epsilon
            }
            sample_points.push_back(sample);
        }
        
        // Add midpoint if range is finite
        if (result_range.lower != -std::numeric_limits<ap_float>::infinity() &&
            result_range.upper != std::numeric_limits<ap_float>::infinity()) {
            sample_points.push_back((result_range.lower + result_range.upper) / 2.0);
        }
        
        // If range extends to infinity, sample some representative points
        if (result_range.lower == -std::numeric_limits<ap_float>::infinity()) {
            sample_points.push_back(-1000000.0);
        }
        if (result_range.upper == std::numeric_limits<ap_float>::infinity()) {
            sample_points.push_back(1000000.0);
        }
        
        // Check partition membership for all sample points
        for (ap_float sample : sample_points) {
            int partition_idx = get_partition_index(sample);
            if (partition_idx >= 0) {
                // Add to reachable if not already present
                if (std::find(reachable_partitions.begin(), reachable_partitions.end(), 
                            partition_idx) == reachable_partitions.end()) {
                    reachable_partitions.push_back(partition_idx);
                }
            }
        }
    }
    
    // Sort for consistency
    std::sort(reachable_partitions.begin(), reachable_partitions.end());
    
    return reachable_partitions;
}

}

