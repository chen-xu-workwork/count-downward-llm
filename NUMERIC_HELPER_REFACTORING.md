# Numeric Helper Refactoring

## Overview
The `DomainAbstractionNumericHelper` has been refactored to work directly with `AbstractOperator` objects from the `DomainAbstractionFactory`, rather than building its own action representations.

## Key Changes

### 1. Removed Duplicate Action Structure
**Before:**
```cpp
struct Action {
    std::vector<FactProxy> propositional_preconditions;
    std::vector<ap_float> additive_effects;
    std::vector<std::pair<int, ap_float>> assignment_effects;
};
```

**After:**
- Removed `Action` struct entirely
- Use `AbstractOperator` objects directly from the factory
- Store a const reference to the abstract operators: `const std::vector<AbstractOperator> &abstract_operators`

### 2. Updated Constructor
**Before:**
```cpp
DomainAbstractionNumericHelper(const std::shared_ptr<AbstractTask> &task);
```

**After:**
```cpp
DomainAbstractionNumericHelper(
    const std::shared_ptr<AbstractTask> &task,
    const std::vector<AbstractOperator> &abstract_operators);
```

The helper now receives the pre-built abstract operators from the factory.

### 3. Removed build_actions() Methods
- Deleted `build_actions()` and `build_action()` methods
- No longer building action representations internally
- Abstract operators are built once by the factory and reused

### 4. Updated Interface
**Changed methods:**
```cpp
// Old:
const Action &get_action(int op_id) const;

// New:
const AbstractOperator &get_abstract_operator(int op_id) const;
```

### 5. Simplified apply_numeric_effects()
```cpp
vector<ap_float> apply_numeric_effects(
    int op_id,
    const vector<ap_float> &current_numeric_state) const {
    
    // Access abstract operator and its concrete counterpart
    const AbstractOperator &abs_op = abstract_operators[op_id];
    int concrete_op_id = abs_op.get_concrete_op_id();
    OperatorProxy op = task_proxy.get_operators()[concrete_op_id];
    
    // Apply numeric effects (TODO: implement full logic)
    // ...
}
```

## Architecture Benefits

### 1. Single Source of Truth
- Abstract operators are built once by `DomainAbstractionFactory`
- No duplication of operator information
- Consistent representation across the codebase

### 2. Clear Separation of Concerns
- **DomainAbstractionFactory**: Builds abstract operators (propositional + numeric effects enumeration)
- **DomainAbstractionNumericHelper**: Handles axiom dependencies and derived variable updates
- **AbstractOperator**: Unified representation with multiple hash effects

### 3. Easier to Maintain
- Changes to operator representation only need to happen in one place
- Clear data flow: Factory → Helper → Abstraction

## Current Status

### ✅ Completed
- Refactored constructor to accept abstract operators
- Removed duplicate Action structure
- Updated interface methods
- Build system integration
- All files compile successfully

### 🚧 TODO (Numeric Axioms Support)
The following methods have stub implementations and need to be completed:

1. **find_derived_numeric_variables()**
   - Detect which numeric variables are derived via assignment axioms
   - Mark them in `is_derived_num_var` vector

2. **build_axiom_dependencies()**
   - Build forward dependencies: `axiom_dependencies[var_id]` = variables that `var_id` depends on
   - Build reverse dependencies: `reverse_axiom_dependencies[var_id]` = derived variables that depend on `var_id`
   - Example:
     ```
     derived1 := x + y
     derived2 := derived1 * 2
     
     axiom_dependencies[derived1] = {x, y}
     axiom_dependencies[derived2] = {derived1}
     reverse_axiom_dependencies[x] = {derived1}
     reverse_axiom_dependencies[derived1] = {derived2}
     ```

3. **compute_derived_updates()**
   - Given a variable change, compute all affected derived variables
   - Return vector of (var_id, new_value) pairs
   - Must respect topological order of axioms

4. **apply_numeric_effects()**
   - Parse assignment effects from operator
   - Apply additive effects
   - Trigger derived variable updates
   - Return new numeric state

5. **parse_arithmetic_expression()**
   - Parse arithmetic expressions used in assignment axioms
   - Build expression tree using `ArithmeticExpression` classes

## Integration with Factory

The typical usage pattern will be:

```cpp
// In DomainAbstractionFactory or CEGAR:

// 1. Build abstract operators
vector<AbstractOperator> operators = compute_abstract_operators(task_proxy, domain_sizes);

// 2. Create numeric helper with the operators
auto numeric_helper = make_shared<DomainAbstractionNumericHelper>(task, operators);

// 3. Use helper to:
//    - Determine which variables are affected by changes
//    - Compute derived variable updates
//    - Check axiom dependencies when refining abstractions
```

## Next Steps

1. **Implement axiom detection**: Parse task to find assignment and comparison axioms
2. **Build dependency graph**: Topological sort of axiom dependencies
3. **Implement derived updates**: Evaluate arithmetic expressions to compute new values
4. **Test with numeric domains**: Verify with problems that have numeric axioms

## References

- Original PDB numeric_helper: `src/search/numeric_pdbs/numeric_helper.h/cc`
- Abstract operators: `src/search/domain_abstractions/domain_abstraction_factory.h`
- Arithmetic expressions: `src/search/numeric_pdbs/arithmetic_expression.h`
