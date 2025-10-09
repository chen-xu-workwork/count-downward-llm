# Abstract Operator Construction for Domain Abstractions

## Overview

This document describes how abstract operators are constructed in the domain abstraction numeric helper, focusing on the handling of numeric variables and their partition transitions.

## Architecture

### Key Components

1. **DomainAbstractionNumericHelper**: Responsible for building all abstract operators
2. **AbstractOperator**: Represents an abstract operator with multiple possible hash effects
3. **NumericDomainMapping**: Manages partitioning of numeric variables into discrete ranges

### Construction Flow

```
build_abstract_operators(task_proxy)
  └─> for each operator in task
       └─> build_abstract_operator(op, operators)
            └─> extract propositional preconditions/effects
            └─> extract numeric assignment effects  
            └─> enumerate_abstract_transitions(...)
                 └─> multiply_out_propositional(...)
                      └─> recursively handle effects without preconditions
                      └─> create AbstractOperator
                           └─> constructor enumerates numeric partition transitions
```

## Implementation Details

### 1. build_abstract_operators()

Main entry point that builds all abstract operators from concrete operators.

```cpp
vector<AbstractOperator> build_abstract_operators(const TaskProxy &task_proxy) {
    vector<AbstractOperator> abstract_operators;
    for (OperatorProxy op : task_proxy.get_operators()) {
        build_abstract_operator(op, abstract_operators);
    }
    return abstract_operators;
}
```

### 2. build_abstract_operator()

Extracts operator information and categorizes facts:

- **prev_pairs**: Prevail conditions (variables that stay equal, needed for regression)
- **pre_pairs**: Preconditions (variables with effects)
- **eff_pairs**: Effects (with corresponding preconditions)
- **effects_without_pre**: Effects without preconditions (need multiply-out)
- **ass_effects**: Numeric assignment effects

Key logic:
```cpp
// Classify each precondition
if (has_effect_on_var[var_id] >= 0) {
    pre_pairs.emplace_back(var_id, val);  // Has effect → precondition
} else {
    prev_pairs.emplace_back(var_id, val);  // No effect → prevail
}
```

### 3. enumerate_abstract_transitions()

Delegates to `multiply_out_propositional()` to handle propositional effects without preconditions.

### 4. multiply_out_propositional()

**Purpose**: Handle effects without preconditions by enumerating all possible precondition values.

**Why needed?**: Consider operator with effect `x = 1` but no precondition on `x`:
- If `domain(x) = {0, 1, 2}`, we create 3 abstract operators:
  - `pre={x=0}, eff={x=1}` (transition 0→1)
  - `pre={x=1}, eff={x=1}` (no change, becomes prevail)
  - `pre={x=2}, eff={x=1}` (transition 2→1)

**Recursion pattern**:
```cpp
multiply_out_propositional(pos, ...) {
    if (pos == effects_without_pre.size()) {
        // Base case: create AbstractOperator
        operators.emplace_back(...);
    } else {
        // Recursive case: try all values for current variable
        for (int i = 0; i < domain_sizes[var_id]; ++i) {
            if (i != eff) {
                pre_pairs.add(var_id, i);
                eff_pairs.add(var_id, eff);
            } else {
                prev_pairs.add(var_id, i);  // Effect equals pre → prevail
            }
            multiply_out_propositional(pos + 1, ...);
            // Backtrack
        }
    }
}
```

### 5. AbstractOperator Constructor (in factory)

The `AbstractOperator` constructor itself handles numeric partition enumeration:

- For each numeric variable with an assignment effect:
  - Enumerate all possible source partitions
  - For each source partition, determine possible target partitions
  - Each combination creates a different `hash_effect`
- Result: One `AbstractOperator` with multiple `hash_effects`

## Numeric Variables and Partitions

### Domain Partitioning

Numeric variables (continuous) are discretized into finite partitions:

```
Variable: x with partitions [0, 5), [5, 10), [10, ∞)
          ↓
Partition indices: 0, 1, 2
```

### Multiple Hash Effects

An operator with numeric effect `x := y + 1` can cause multiple abstract transitions:

```
If y can be in partitions 0 or 1:
  y ∈ [0, 5)   → x ∈ [1, 6)   → might transition to partition 0 or 1
  y ∈ [5, 10)  → x ∈ [6, 11)  → might transition to partition 1 or 2
  
Result: Multiple hash_effects, one per (source_partition, target_partition) pair
```

## Cascading Effects (Future Work)

### Assignment Axioms

```
derived := x + y
```
When `x` changes partition, `derived` may also change partition.

### Comparison Axioms  

```
(x > 5) creates a derived propositional variable
```
When `x` transitions from partition [0,5) to [5,10), the derived variable flips from false to true.

### Implementation Plan

1. **find_derived_variables()**: Detect axioms and mark derived variables
2. **build_axiom_dependencies()**: Build dependency graph
3. **compute_affected_comparison_axioms()**: Compute cascading propositional effects
4. **enumerate_abstract_transitions()**: Integrate cascade computation

## Key Differences from PDB Numeric Helper

| Aspect | PDB Helper | Domain Abstraction Helper |
|--------|-----------|--------------------------|
| Variables | Works with pattern (subset) | Works with ALL variables |
| Abstraction | Fixed projection | Abstraction via domain mapping |
| Operators | Builds own Action type | Builds AbstractOperators |
| Cascades | Not needed (pattern-based) | Essential (all-variable) |
| Output | Actions for PDB construction | AbstractOperators for CEGAR |

## Current Status

✅ **Implemented**:
- Constructor with domain mapping parameters
- `build_abstract_operators()` main method
- `build_abstract_operator()` extraction logic
- `enumerate_abstract_transitions()` delegation
- `multiply_out_propositional()` recursion pattern
- Proper AbstractOperator creation

⏳ **TODO**:
- Implement axiom detection (`find_derived_variables()`)
- Implement dependency tracking (`build_axiom_dependencies()`)
- Implement cascade computation (`compute_affected_comparison_axioms()`)
- Integrate cascades into operator construction
- Update factory to use numeric helper

## Testing

To test the implementation:

1. Build: `./build.py release64`
2. Run with domain abstraction heuristic on numeric domain
3. Verify abstract operators are created correctly
4. Check that numeric partition transitions are enumerated

## References

- `domain_abstraction_factory.cc`: Original multiply_out and build_abstract_operators
- `AbstractOperator` constructor: Numeric partition enumeration
- PDB numeric helper: Similar axiom handling patterns (to be adapted)
