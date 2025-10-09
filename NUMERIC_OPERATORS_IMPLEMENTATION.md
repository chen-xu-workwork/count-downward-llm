# Numeric Operators Implementation for Domain Abstractions

## Overview
This document describes how domain abstractions handle numeric effects and the challenge of multiple successor states from a single abstract state.

## Problem Statement
When numeric variables are discretized into partitions, numeric effects (like `x += 2`) can cause transitions between different partitions. This creates a key challenge:

**A single abstract operator can have multiple hash effects (one per possible abstract successor state).**

### Example
Consider:
- Numeric variable `a` abstracted into ranges: `[-inf, 0)` (partition 0) and `[0, inf)` (partition 1)
- Operator with effect: `a += 2`
- Current abstract state: `a` is in partition 0

**Issue**: Depending on the concrete value of `a`:
- If `a = -3`, after `a += 2`, we get `a = -1` → still partition 0
- If `a = -1`, after `a += 2`, we get `a = 1` → transitions to partition 1

So from abstract state with partition 0, we can reach BOTH partition 0 AND partition 1.

**Solution**: The abstract operator stores multiple hash effects - one for each possible target partition.

## Implementation Approach

### Key Design Decisions

1. **Multiple Hash Effects per Operator**: Abstract operators store a vector of hash effects instead of a single value
2. **Enumeration at Operator Construction Time**: During `build_abstract_operators`, we enumerate all possible partition transitions for numeric effects
3. **Uniform Treatment**: Both propositional and numeric operators use the same `AbstractOperator` class and regression framework
4. **Finite State Space Assumption**: We work with a finite number of partitions, enabling perfect hashing like propositional variables

### Code Structure

#### 1. AbstractOperator Structure
```cpp
class AbstractOperator {
    std::vector<int> hash_effects;  // Multiple effects for numeric operators
    // ...
};
```

- **Propositional-only operators**: `hash_effects` contains 1 element
- **Operators with numeric effects**: `hash_effects` contains multiple elements (one per possible target partition combination)

#### 2. Hash Multipliers for Numeric Variables
```cpp
// Add hash multipliers for numeric variables (after propositional vars)
for (size_t i = 0; i < numeric_domain_mapping.size(); ++i) {
    hash_multipliers.push_back(num_states);
    int num_partitions = numeric_domain_sizes[i];
    num_states *= num_partitions;
}
```

#### 3. Abstract Operator Construction
In `AbstractOperator` constructor:
```cpp
// Check if this operator has numeric effects
if (!has_numeric_effects || numeric_domain_mapping.empty()) {
    // Propositional-only operator: single hash effect
    hash_effects.push_back(base_hash_effect);
} else {
    // Operator with numeric effects: enumerate all possible hash effects
    // For each affected numeric variable, try all target partitions
    // This creates the cartesian product of all possible transitions
}
```

The enumeration happens recursively:
- For each numeric variable affected by the operator
- We consider ALL possible target partitions (conservative overapproximation)
- Each combination generates one hash effect

#### 4. Regression Search (Dijkstra)
During `compute_distances`, all operators are handled uniformly:

```cpp
vector<int> applicable_operator_ids;
match_tree.get_applicable_operator_ids(state_index, applicable_operator_ids);
for (int op_id : applicable_operator_ids) {
    const AbstractOperator &op = operators[op_id];
    
    // Iterate over ALL hash effects
    // Propositional: 1 effect, Numeric: multiple effects
    for (int hash_effect : op.get_hash_effects()) {
        int predecessor = state_index + hash_effect;
        // Update distance if improved
    }
}
```

No special handling needed - the abstract operator encodes all possible transitions!

## Differences from Pattern Databases

| Aspect | Pattern Databases | Domain Abstractions |
|--------|------------------|---------------------|
| **Variables** | Subset (pattern) | All variables (abstracted) |
| **Numeric Values** | Exact continuous values | Discretized partitions |
| **State Representation** | `prop_hash + vector<ap_float>` | Single combined hash |
| **Numeric Effects** | Computed during forward search | Considered during regression |
| **Multiple Successors** | Handled in forward exploration | Handled as multiple predecessors in regression |

## Conservative Overapproximation

The current implementation uses a **conservative overapproximation** for numeric effects:
- For each affected numeric variable, we consider ALL partitions as possible predecessors
- This ensures correctness (admissibility) but may reduce heuristic quality

### Example of Overapproximation
- Operator: `x += 2`
- Variable `x` has 3 partitions: `[-inf, 0)`, `[0, 10)`, `[10, inf)`
- Current state: `x` in partition `[0, 10)`
- **Conservative approach**: Predecessor could have `x` in ANY of the 3 partitions
- **More precise approach**: Would analyze the effect magnitude and range bounds

## Future Improvements

1. **Refined Predecessor Computation**: Analyze effect magnitude and partition boundaries to compute tighter predecessor sets
2. **Symbolic Representation**: Use constraints to represent reachable predecessor regions
3. **Operator Splitting**: Create multiple abstract operators for different partition transitions
4. **Additive Effects Analysis**: Distinguish between assignment (`x = 5`) and additive (`x += 2`) effects

## Current Limitations

1. **Overapproximation**: May generate more predecessors than necessary
2. **No Goal Support**: Numeric goal conditions not yet implemented
3. **No Refinement**: CEGAR refinement doesn't split numeric ranges yet
4. **Plan Extraction**: Abstract plans with numeric operators need special handling

## Testing Strategy

To test this implementation:
1. Create simple domains with numeric variables
2. Define clear partition boundaries
3. Test operators that cross partition boundaries
4. Verify heuristic values are admissible
5. Check for completeness (no states missed in regression)

## Related Files

- `domain_abstraction_factory.h/cc`: Main implementation
- `types.h/cc`: NumericDomainMapping and partition management
- `domain_abstraction.h/cc`: Heuristic computation with numeric variables
- `domain_abstraction_state_registry.h/cc`: State storage for mixed prop/numeric states
