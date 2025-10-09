# Axiom Detection and Cascade Handling Implementation

## Overview

This document describes the implementation of axiom detection and cascade computation for domain abstractions with numeric variables. The system handles two types of axioms:

1. **Assignment Axioms**: Create derived numeric variables (e.g., `derived := x + y`)
2. **Comparison Axioms**: Create derived propositional variables (e.g., `(x > 5)` creates a boolean)

## Architecture

### Data Structures

```cpp
// Track which numeric variables are derived from assignment axioms
std::vector<bool> is_derived_num_var;

// Track which propositional variables are derived from comparison axioms
std::vector<bool> is_derived_prop_var;

// Forward dependencies: derived_var -> [source_var1, source_var2, ...]
std::vector<std::vector<int>> axiom_dependencies;

// Reverse dependencies: source_var -> [derived_var1, derived_var2, ...]
std::vector<std::vector<int>> reverse_axiom_dependencies;
```

### Key Methods

1. **find_derived_variables()**: Detect all axioms and mark derived variables
2. **build_axiom_dependencies()**: Build dependency graphs
3. **compute_affected_comparison_axioms()**: Compute cascading propositional effects

## Implementation Details

### 1. find_derived_variables()

**Purpose**: Identify all derived variables from task axioms.

**Assignment Axioms**:
```cpp
AssignmentAxiomsProxy assignment_axioms = task_proxy.get_assignment_axioms();
for (AssignmentAxiomProxy axiom : assignment_axioms) {
    NumericVariableProxy derived_var = axiom.get_assignment_variable();
    int derived_id = derived_var.get_id();
    is_derived_num_var[derived_id] = true;
}
```

Each assignment axiom has:
- `get_left_variable()`: Left operand
- `get_right_variable()`: Right operand
- `get_arithmetic_operator_type()`: Operator (sum, diff, mult, divi)
- `get_assignment_variable()`: Target derived variable

**Comparison Axioms**:
```cpp
ComparisonAxiomsProxy comparison_axioms = task_proxy.get_comparison_axioms();
for (ComparisonAxiomProxy axiom : comparison_axioms) {
    FactProxy true_fact = axiom.get_true_fact();
    FactProxy false_fact = axiom.get_false_fact();
    int var_id = true_fact.get_variable().get_id();
    is_derived_prop_var[var_id] = true;
}
```

Each comparison axiom has:
- `get_left_variable()`: Left operand (numeric)
- `get_right_variable()`: Right operand (numeric)
- `get_comparison_operator_type()`: Operator (lt, le, eq, ge, gt, ue)
- `get_true_fact()`: Fact when comparison is true
- `get_false_fact()`: Fact when comparison is false

### 2. build_axiom_dependencies()

**Purpose**: Build forward and reverse dependency graphs for assignment axioms.

**Forward Dependencies** (derived → sources):
```
derived := x + y
  → axiom_dependencies[derived] = [x, y]
```

**Reverse Dependencies** (source → deriveds):
```
x affects derived1, derived2
  → reverse_axiom_dependencies[x] = [derived1, derived2]
```

**Implementation**:
```cpp
for (AssignmentAxiomProxy axiom : assignment_axioms) {
    int derived_id = axiom.get_assignment_variable().get_id();
    int left_id = axiom.get_left_variable().get_id();
    int right_id = axiom.get_right_variable().get_id();
    
    // Forward: derived depends on left and right
    axiom_dependencies[derived_id].push_back(left_id);
    axiom_dependencies[derived_id].push_back(right_id);
    
    // Reverse: left and right affect derived
    reverse_axiom_dependencies[left_id].push_back(derived_id);
    reverse_axiom_dependencies[right_id].push_back(derived_id);
}
```

**Note**: Comparison axioms create propositional variables (not numeric), so they're not included in the numeric dependency graph. They're handled separately in cascade computation.

### 3. compute_affected_comparison_axioms()

**Purpose**: Determine which comparison axioms (propositional derived variables) change truth values when numeric variables change partitions.

**Input**:
- `changed_numeric_vars`: IDs of numeric variables that changed
- `old_partitions`: Previous partition indices for each changed variable
- `new_partitions`: New partition indices for each changed variable

**Output**:
- Vector of `Fact` objects representing propositional variables that may have changed

**Algorithm**:

```cpp
for each comparison_axiom:
    left_var = axiom.get_left_variable()
    right_var = axiom.get_right_variable()
    
    if (left_var or right_var in changed_numeric_vars):
        // Check if partition changed for operands
        if (partition changed for left_var or right_var):
            // Conservative: assume truth value might change
            add true_fact and false_fact to affected_facts
```

**Conservative Approach**: Currently, if any operand of a comparison changes partition, we conservatively assume the truth value might change. This ensures correctness at the cost of potentially over-approximating effects.

**Future Refinement**: Could compute exact truth value by:
1. Getting the partition ranges for old and new partitions
2. Evaluating the comparison operator for all value combinations
3. Determining if the truth value definitely changes, stays the same, or is uncertain

## Example Scenarios

### Scenario 1: Assignment Axiom Chain

```
x = regular variable
derived1 := x + 5
derived2 := derived1 * 2
```

**Dependency Graph**:
```
axiom_dependencies[derived1] = [x]
axiom_dependencies[derived2] = [derived1]
reverse_axiom_dependencies[x] = [derived1]
reverse_axiom_dependencies[derived1] = [derived2]
```

**Cascade**: When `x` changes → `derived1` changes → `derived2` changes

### Scenario 2: Comparison Axiom

```
x = regular variable (partitions: [0,5), [5,10), [10,∞))
comparison: (x >= 5)
derived_prop = propositional variable for the comparison result
```

**Marking**:
```
is_derived_prop_var[derived_prop] = true
```

**Cascade**:
- `x` transitions from partition 0 ([0,5)) to partition 1 ([5,10))
  - Old: x ∈ [0,5) → comparison false
  - New: x ∈ [5,10) → comparison true
  - Result: `derived_prop` changes from false to true

### Scenario 3: Mixed Axioms

```
x = regular variable
y = regular variable
sum := x + y          (assignment axiom)
condition := (sum > 10)   (comparison axiom)
```

**Marking**:
```
is_derived_num_var[sum] = true
is_derived_prop_var[condition] = true
```

**Dependencies**:
```
axiom_dependencies[sum] = [x, y]
reverse_axiom_dependencies[x] = [sum]
reverse_axiom_dependencies[y] = [sum]
```

**Cascade**:
1. `x` changes partition
2. `sum` must be recomputed (numeric cascade)
3. `condition` depends on `sum` (comparison cascade)
4. `condition` propositional value may change

## Integration with Operator Construction

### Current Status

The axiom detection and dependency tracking are now implemented. The next step is integrating cascade computation into operator construction.

### Integration Points

1. **In `enumerate_abstract_transitions()`**:
   - When creating operators with numeric effects
   - For each numeric variable effect, enumerate partition transitions
   - For each transition, call `compute_affected_comparison_axioms()`
   - Include the affected propositional facts in the operator's effects

2. **In `AbstractOperator` constructor**:
   - Already enumerates numeric partition transitions
   - Need to add: For each numeric transition, compute cascading comparison effects
   - Result: Each hash_effect includes both numeric and propositional changes

### Pseudocode for Full Cascade

```cpp
for each numeric_effect in operator:
    for each possible (old_partition, new_partition):
        // Direct numeric effect
        hash_offset = compute_numeric_hash_offset(var, old_part, new_part)
        
        // Assignment axiom cascades (numeric derived variables)
        affected_derived_numeric = compute_derived_numeric_updates(var, old_part, new_part)
        for each derived_var in affected_derived_numeric:
            hash_offset += compute_numeric_hash_offset(derived_var, ...)
        
        // Comparison axiom cascades (propositional derived variables)
        affected_facts = compute_affected_comparison_axioms([var], [old_part], [new_part])
        for each fact in affected_facts:
            hash_offset += compute_propositional_hash_offset(fact)
        
        hash_effects.push_back(hash_offset)
```

## Testing Strategy

### Unit Tests

1. **Test axiom detection**:
   - Create task with assignment axioms
   - Verify `is_derived_num_var` correctly marked
   - Create task with comparison axioms
   - Verify `is_derived_prop_var` correctly marked

2. **Test dependency building**:
   - Create axiom chain: `a := x`, `b := a`, `c := b`
   - Verify forward dependencies
   - Verify reverse dependencies

3. **Test cascade computation**:
   - Create comparison axiom: `(x > 5)`
   - Test partition transition [0,5) → [5,10)
   - Verify affected fact is returned

### Integration Tests

1. **Simple numeric domain**:
   - Domain with one numeric variable
   - One operator affecting that variable
   - Verify abstract operators created

2. **Domain with assignment axioms**:
   - Regular variable and derived variable
   - Operator affecting regular variable
   - Verify cascade to derived variable

3. **Domain with comparison axioms**:
   - Numeric variable with partitions
   - Comparison axiom creating propositional variable
   - Verify propositional effects in abstract operators

## Performance Considerations

### Time Complexity

- **find_derived_variables()**: O(#assignment_axioms + #comparison_axioms)
- **build_axiom_dependencies()**: O(#assignment_axioms)
- **compute_affected_comparison_axioms()**: O(#comparison_axioms × #changed_vars)

### Space Complexity

- **Dependency graphs**: O(#numeric_vars × avg_dependencies)
- **Per operator**: O(#hash_effects), where hash_effects can grow exponentially with numeric variables

### Optimization Opportunities

1. **Exact comparison evaluation**: Instead of conservative over-approximation, compute exact truth values when possible

2. **Caching**: Cache comparison evaluations for partition pairs

3. **Pruning**: If a comparison's truth value cannot change (e.g., both operands in same partition, no transition affects comparison), skip it

4. **Incremental updates**: When updating derived variables, use topological ordering to minimize redundant computations

## Current Status

✅ **Implemented**:
- Axiom detection for both types
- Dependency graph construction
- Conservative cascade computation for comparison axioms
- Public API for checking derived variables

⏳ **TODO**:
- Integrate cascade computation into operator enumeration
- Implement exact comparison evaluation
- Handle transitive cascades (assignment → comparison)
- Optimize cascade computation
- Add comprehensive testing

## References

- Task proxy axiom APIs: `task_proxy.h`
- Operator construction: `numeric_helper.cc::build_abstract_operator()`
- AbstractOperator: `domain_abstraction_factory.h`
- Comparison/assignment operators: `globals.h`
