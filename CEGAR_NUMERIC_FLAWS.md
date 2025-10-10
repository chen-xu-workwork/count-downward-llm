# CEGAR with Numeric Variables: Flaw Detection and Refinement

## Table of Contents
1. [Overview of CEGAR for Domain Abstractions](#overview)
2. [Propositional CEGAR: Current Implementation](#propositional-cegar)
3. [The Numeric Variable Challenge](#numeric-challenge)
4. [Proposed Solutions for Numeric Flaws](#proposed-solutions)
5. [Implementation Considerations](#implementation)

---

## Overview of CEGAR for Domain Abstractions {#overview}

**CEGAR** (Counter-Example Guided Abstraction Refinement) is an iterative refinement algorithm that computes abstractions by:

1. **Start coarse**: Begin with a very abstract (coarse) state space
2. **Find plan**: Compute an optimal plan in the abstract space
3. **Verify plan**: Try to execute the abstract plan in the concrete state space
4. **Detect flaws**: If execution fails, identify why (the "flaw")
5. **Refine**: Split abstract values to distinguish the problematic cases
6. **Repeat**: Until a valid concrete plan is found or refinement is impossible

### Key Insight: Domain Abstractions vs Pattern Databases

- **Pattern Databases**: Project away variables (ignore some variables entirely)
- **Domain Abstractions**: Keep all variables but merge their values into partitions

**Advantage**: Domain abstractions can represent relationships between variables more naturally while still achieving abstraction through value merging.

---

## Propositional CEGAR: Current Implementation {#propositional-cegar}

### Partition Representation

For each propositional variable `v` with concrete domain `{0, 1, ..., n-1}`, we maintain a **domain mapping**:

```
dom_mapping[v] : concrete_value → abstract_value
```

**Example**:
```
Variable v with domain {0, 1, 2, 3, 4}

Initial (coarsest) abstraction:
  dom_mapping[v] = [0, 0, 0, 0, 0]
  All concrete values map to abstract value 0

After first refinement (split value 2):
  dom_mapping[v] = [0, 0, 1, 0, 0]
  Value 2 now has its own partition
  
After second refinement (split value 0):
  dom_mapping[v] = [2, 0, 1, 0, 0]
  Value 0 also has its own partition
```

### Abstract State Space

An **abstract state** is a tuple of abstract values:
```
s_abstract = (v₁_abstract, v₂_abstract, ..., vₙ_abstract)
```

The **abstract state space size** is:
```
|S_abstract| = ∏ᵢ |dom_abstract(vᵢ)|
```

### Flaw Types

#### 1. Precondition Flaw

**Scenario**: 
- Abstract plan says: apply operator `o` 
- Operator `o` has precondition `v = x`
- In concrete state: `v = y` (where `y ≠ x`)
- But: `dom_mapping[v][x] == dom_mapping[v][y]` (same abstract value!)

**Problem**: The abstraction cannot distinguish between `x` and `y`, so it thinks the operator is applicable when it's not.

**Solution**: **Refine variable `v` at value `x`**:
```
Before: dom_mapping[v][x] = dom_mapping[v][y] = 0
After:  dom_mapping[v][x] = 1, dom_mapping[v][y] = 0
```

Now the abstraction can distinguish states where `v=x` from states where `v=y`.

#### 2. Goal Flaw

**Scenario**:
- Abstract plan successfully executes in concrete space
- But final concrete state does not satisfy all goals
- Goal `g = w` is not true (concrete state has `g = z`)
- But: `dom_mapping[g][w] == dom_mapping[g][z]` (same abstract value!)

**Problem**: The abstraction thinks it reached the goal when it actually didn't.

**Solution**: **Refine variable `g` at value `w`** (the goal value):
```
Before: dom_mapping[g][w] = dom_mapping[g][z] = 0
After:  dom_mapping[g][w] = 1, dom_mapping[g][z] = 0
```

### CEGAR Loop (Propositional)

```
1. Initialize: dom_mapping[v][i] = 0 for all v, i
2. Build abstraction from domain mapping
3. Compute optimal abstract plan π_abstract
4. Try to execute π_abstract in concrete state space:
   
   concrete_state := initial_state
   for each operator o in π_abstract:
       # Check preconditions
       for each precondition (v, x) of o:
           if concrete_state[v] ≠ x:
               FLAW: precondition flaw on (v, x)
               Refine dom_mapping[v] at value x
               GOTO 2
       
       # Apply operator
       concrete_state := apply(o, concrete_state)
   
   # Check goals
   for each goal (g, w):
       if concrete_state[g] ≠ w:
           FLAW: goal flaw on (g, w)
           Refine dom_mapping[g] at value w
           GOTO 2
   
   # Success!
   return π_abstract (it's also a valid concrete plan)
```

---

## The Numeric Variable Challenge {#numeric-challenge}

### Problem Statement

Numeric variables have **continuous or very large domains**. We cannot enumerate all values explicitly like we do for propositional variables.

**Example**:
```
Numeric variable: fuel ∈ [0, 100] (or even ℝ)
```

We currently initialize numeric variables with:
```
numeric_domain_mapping[fuel] = single range (-∞, ∞) → partition 0
```

### Key Questions

1. **What is a numeric flaw?**
   - Precondition flaw: How do we represent "precondition not satisfied" for numeric conditions?
   - Goal flaw: How do we represent "goal not satisfied" for numeric goals?

2. **How do we refine numeric partitions?**
   - Propositional: Split a single value into its own partition
   - Numeric: Split a range at some threshold value

3. **Where do we split?**
   - What threshold value should we choose for splitting?

### Current Architecture

**Numeric variables are already discretized into partitions:**

```cpp
class NumericDomainMapping {
    std::vector<NumericRange> ranges;  // Sorted, cover (-∞, ∞)
    
    // Initially: ranges = [Range(-∞, ∞, partition=0)]
    // After split at 50: ranges = [Range(-∞, 50, partition=0),
    //                              Range(50, ∞, partition=1)]
};
```

**Numeric goals are compiled to comparison axioms:**
- Numeric goal: `fuel ≥ 50`
- Becomes: Derived propositional variable `goal_fuel_ge_50` that is true iff `fuel ≥ 50`

**Numeric preconditions are also comparison axioms:**
- Numeric precondition: `distance < 10`
- Becomes: Derived propositional variable that must be true

---

## Proposed Solutions for Numeric Flaws {#proposed-solutions}

### Solution 1: Treat Comparison Axioms as Regular Variables (Current Partial Support)

**Idea**: Since numeric conditions are compiled to propositional variables, we can use the existing propositional flaw mechanism.

#### Precondition Flaws

**Scenario**:
- Operator requires: `comp_var := (fuel ≥ 50)` must be true
- In abstract state: `comp_var` could be true OR false (partition spans threshold)
- In concrete state: `fuel = 30`, so `comp_var = false`
- Operator is not applicable!

**Flaw Detection**:
```cpp
for each precondition comp_var = true of operator:
    if concrete_state[comp_var] == false:
        FLAW: precondition flaw on (comp_var, true)
```

**Refinement**:
- Option A: Split the comparison variable (but this is derived, doesn't help much)
- Option B: **Split the underlying numeric variable at the threshold**

**Option B Details**:
```
Comparison: (fuel ≥ 50)
Current partition: fuel ∈ (-∞, ∞) → partition 0

After refinement at threshold 50:
  fuel ∈ (-∞, 50) → partition 0
  fuel ∈ [50, ∞)  → partition 1

Now the comparison axiom can be evaluated exactly:
  - If fuel in partition 0: comp_var = false (definitely)
  - If fuel in partition 1: comp_var = true (definitely)
```

#### Goal Flaws

**Scenario**:
- Goal: `goal_comp := (fuel ≥ 50)` must be true
- After executing plan: `fuel = 30`
- So: `goal_comp = false` (goal not satisfied!)

**Flaw Detection**:
```cpp
for each goal comp_var = true:
    if concrete_state[comp_var] == false:
        FLAW: goal flaw on (comp_var, true)
```

**Refinement**: Same as precondition flaws - split the underlying numeric variable.

### Solution 2: Direct Numeric Flaw Representation

**Idea**: Introduce explicit numeric flaw types that directly reference numeric variables and thresholds.

#### New Flaw Types

```cpp
struct NumericPreconditionFlaw {
    int numeric_var_id;
    ComparisonOperator op;  // <, ≤, >, ≥
    ap_float threshold;
    ap_float concrete_value;
    
    // Example: (fuel ≥ 50) required, but fuel = 30
    // numeric_var_id = fuel
    // op = ≥
    // threshold = 50
    // concrete_value = 30
};

struct NumericGoalFlaw {
    int numeric_var_id;
    ComparisonOperator op;
    ap_float threshold;
    ap_float concrete_value;
};
```

**Refinement Strategy**

When we detect a numeric flaw, we need to split carefully. There are several options:

#### Option A: Split at Concrete Value (Recommended)

Split at the **concrete value** that caused the flaw:

```cpp
void refine_numeric_variable(int var_id, ap_float concrete_value) {
    numeric_domain_mapping[var_id].split_at(concrete_value);
    numeric_domain_sizes[var_id] = 
        numeric_domain_mapping[var_id].get_num_partitions();
}
```

**Example**:
```
Flaw: Operator requires (fuel ≥ 50), but concrete state has fuel = 30

Before refinement:
  numeric_domain_mapping[fuel] has 1 partition:
    Range(-∞, ∞) → partition 0

Call: refine_numeric_variable(fuel, 30)

After refinement:
  numeric_domain_mapping[fuel] has 2 partitions:
    Range(-∞, 30) → partition 0
    Range[30, ∞)  → partition 1

Result:
  - fuel = 30 → partition 1: [30, ∞)
  - Operator fuel += 1 from partition 1 → partition 1 (stays in [30, ∞))
  
This doesn't immediately solve the fuel ≥ 50 problem, but:
  - Progressive refinement: Next iteration might split at 50
  - Distinguishes states that are definitely far from satisfying precondition
```

**Why this works over time**:
- Each flaw detection identifies a problematic concrete value
- Splitting at that value refines the abstraction incrementally
- After enough iterations, we have sufficient partitions to distinguish all relevant cases

#### Option B: Split at Both Concrete Value and Threshold (Aggressive)

Split at **both** the concrete value AND the threshold:

```cpp
void refine_numeric_variable_aggressive(
    int var_id, 
    ap_float concrete_value,
    ap_float threshold) {
    
    // Split at concrete value
    numeric_domain_mapping[var_id].split_at(concrete_value);
    
    // Split at threshold (might split an already-split range)
    numeric_domain_mapping[var_id].split_at(threshold);
    
    numeric_domain_sizes[var_id] = 
        numeric_domain_mapping[var_id].get_num_partitions();
}
```

**Example**:
```
Flaw: Operator requires (fuel ≥ 50), but concrete state has fuel = 30

Split at both 30 and 50:
  numeric_domain_mapping[fuel] has 3 partitions:
    Range(-∞, 30)  → partition 0
    Range[30, 50)  → partition 1
    Range[50, ∞)   → partition 2

Now:
  - fuel = 30 → partition 1: [30, 50)
  - Operator requiring fuel ≥ 50 is ONLY applicable from partition 2!
  - The abstraction knows partition 1 cannot satisfy this precondition
```

**Trade-off**:
- ✅ Pro: Faster convergence (fewer CEGAR iterations)
- ✅ Pro: Immediately distinguishes problematic cases
- ❌ Con: Creates more partitions per iteration (larger abstract state space)
- ❌ Con: Might exceed abstraction size limit sooner

#### Option C: Split at Threshold Only (Not Recommended)

This was my initial suggestion, but **you're right that it doesn't work**:

```
Split at threshold 50:
  Range(-∞, 50) → partition 0
  Range[50, ∞)  → partition 1

Problem:
  - fuel = 30 is still in partition 0
  - Operator fuel += 1 can transition from partition 0 to either 0 or 1
  - We still can't tell if fuel=30 can satisfy fuel ≥ 50 after the operator!
  - Same flaw would occur in next iteration!
```

This doesn't help because the concrete value causing the flaw is still lumped together with many other values in the same partition.

### Solution 3: Hybrid Approach (Recommended)

**Combine both solutions**:

1. **Use existing comparison axiom infrastructure** for detecting flaws
   - Flaws are detected on derived propositional variables
   - No need to change flaw detection logic significantly

2. **Trace back to numeric variables for refinement**
   - When a flaw is detected on a comparison axiom `c := (x op threshold)`
   - Split the underlying numeric variable `x` at `threshold`

3. **Maintain mapping from comparison axioms to numeric conditions**
   ```cpp
   struct ComparisonAxiomInfo {
       int numeric_var_id;
       ComparisonOperator op;
       ap_float threshold;
   };
   
   // Map: propositional_var_id → numeric condition info
   unordered_map<int, ComparisonAxiomInfo> comparison_axiom_map;
   ```

#### Algorithm

```cpp
struct NumericRefinement {
    int numeric_var_id;
    ap_float concrete_value;  // The concrete value that caused the flaw
    ap_float threshold;       // The threshold from the comparison (optional, for aggressive refinement)
};

vector<NumericRefinement> get_numeric_flaws(
    const TaskProxy &task_proxy,
    const State &concrete_init,
    const DomainAbstraction &abstraction) {
    
    vector<NumericRefinement> refinements;
    vector<int> concrete_state = get_prop_state(concrete_init);
    vector<ap_float> numeric_state = get_numeric_state(concrete_init);
    
    vector<vector<int>> plan = abstraction.get_plan();
    
    for (vector<int> &equivalent_ops : plan) {
        for (int op_id : equivalent_ops) {
            OperatorProxy op = task_proxy.get_operators()[op_id];
            
            // Check propositional preconditions
            vector<Fact> prop_flaws = 
                get_precondition_flaws(op, concrete_state, ...);
            
            // Check numeric preconditions (via comparison axioms)
            for (FactProxy pre : op.get_preconditions()) {
                if (is_comparison_axiom(pre.get_variable())) {
                    int prop_var = pre.get_variable().get_id();
                    int required_value = pre.get_value();
                    int actual_value = concrete_state[prop_var];
                    
                    if (required_value != actual_value) {
                        // Flaw on comparison axiom!
                        // Get underlying numeric condition
                        ComparisonAxiomInfo info = 
                            comparison_axiom_map[prop_var];
                        
                        // Get the CONCRETE VALUE that caused the flaw
                        ap_float concrete_value = 
                            numeric_state[info.numeric_var_id];
                        
                        refinements.push_back({
                            info.numeric_var_id,
                            concrete_value,  // Split at concrete value
                            info.threshold   // Optionally also split at threshold
                        });
                    }
                }
            }
            
            if (flaws.empty()) {
                apply_op_to_state(concrete_state, numeric_state, op);
                break;
            }
        }
        if (!refinements.empty()) {
            return refinements;
        }
    }
    
    // Check goals
    for (FactProxy goal : task_proxy.get_goals()) {
        if (is_comparison_axiom(goal.get_variable())) {
            int prop_var = goal.get_variable().get_id();
            int required_value = goal.get_value();
            int actual_value = concrete_state[prop_var];
            
            if (required_value != actual_value) {
                ComparisonAxiomInfo info = 
                    comparison_axiom_map[prop_var];
                
                // Get the CONCRETE VALUE that caused the goal flaw
                ap_float concrete_value = 
                    numeric_state[info.numeric_var_id];
                
                refinements.push_back({
                    info.numeric_var_id,
                    concrete_value,
                    info.threshold
                });
            }
        }
    }
    
    return refinements;
}
```

---

## Implementation Considerations {#implementation}

### 1. Identifying Comparison Axioms

**Need to distinguish** derived propositional variables that come from comparison axioms vs. regular derived variables.

**Options**:
- A. Add metadata during task construction
- B. Check variable name patterns (e.g., starts with "comp_")
- C. Maintain explicit list of comparison axiom variable IDs

**Recommendation**: Option C (explicit list) - most robust.

### 2. Extracting Threshold Values

From a comparison axiom, we need to extract:
- The numeric variable being compared
- The comparison operator
- The threshold value

**Implementation**:
```cpp
struct ComparisonAxiomInfo {
    int numeric_var_id;
    ComparisonOperator op;
    ap_float threshold;
    int true_value;   // propositional value when condition is true
    int false_value;  // propositional value when condition is false
};

void build_comparison_axiom_map() {
    comparison_axiom_map.clear();
    
    ComparisonAxiomsProxy axioms = task_proxy.get_comparison_axioms();
    for (ComparisonAxiomProxy axiom : axioms) {
        int prop_var_id = axiom.get_true_fact().get_variable().get_id();
        
        // Extract numeric condition information
        // (depends on how comparison axioms are represented)
        int numeric_var = extract_numeric_var(axiom);
        ComparisonOperator op = extract_operator(axiom);
        ap_float threshold = extract_threshold(axiom);
        
        comparison_axiom_map[prop_var_id] = {
            numeric_var, op, threshold,
            axiom.get_true_fact().get_value(),
            axiom.get_false_fact().get_value()
        };
    }
}
```

### 3. Refinement Granularity

**Question**: Should we split **all** flawed variables, or just one?

**Options**:
- A. Split all flawed numeric variables at once
- B. Split one random flawed numeric variable
- C. Split the "most refined" variable (similar to propositional case)

**Recommendation**: Start with option A (split all), can experiment with B/C later.

### 4. Abstraction Size Limit

Splitting numeric variables increases abstract state space size:

```
Before split: size = k
After split:  size = k * 2  (if one numeric var splits from 1 to 2 partitions)
```

**Need to check**: Does the split exceed `max_abstraction_size`?

```cpp
bool can_refine_numeric_variable(
    int old_abstraction_size, int numeric_var_id) {
    
    int current_partitions = 
        numeric_domain_mapping[numeric_var_id].get_num_partitions();
    
    int abs_size_without_var = 
        old_abstraction_size / current_partitions;
    
    if (utils::is_product_within_limit(
            abs_size_without_var, 
            current_partitions + 1,  // Will create one more partition
            max_abstraction_size)) {
        return true;
    }
    
    // Blacklist this numeric variable
    blacklisted_numeric_variables.insert(numeric_var_id);
    return false;
}
```

### 5. Plan Execution with Numeric Variables

Need to update `get_flaws()` to:
- Track numeric state alongside propositional state
- Apply numeric effects of operators
- Evaluate comparison axioms based on numeric state

```cpp
void apply_op_to_state(
    vector<int> &prop_state,
    vector<ap_float> &numeric_state,
    const OperatorProxy &op) {
    
    // Apply propositional effects
    for (EffectProxy effect : op.get_effects()) {
        FactProxy fact = effect.get_fact();
        prop_state[fact.get_variable().get_id()] = fact.get_value();
    }
    
    // Apply numeric effects
    NumericEffectsProxy num_effects = op.get_numeric_effects();
    for (NumAssProxy effect : num_effects) {
        int var_id = effect.get_affected_variable().get_id();
        ap_float operand = evaluate(effect.get_assigned_variable(), numeric_state);
        
        switch (effect.get_assign_type()) {
            case assign:
                numeric_state[var_id] = operand;
                break;
            case increase:
                numeric_state[var_id] += operand;
                break;
            case decrease:
                numeric_state[var_id] -= operand;
                break;
            // ... other operators
        }
    }
    
    // Update comparison axioms based on new numeric state
    update_comparison_axioms(prop_state, numeric_state);
}
```

### 6. Multiple Refinements from One Flaw

A single operator might have multiple numeric preconditions that fail:
```
Operator: fly(from, to)
  Requires: fuel ≥ 50 AND distance < 100
  Concrete: fuel = 30, distance = 120
  Both preconditions fail!
```

**Options**:
- A. Refine all flawed variables
- B. Refine one random flawed variable
- C. Refine based on some heuristic (e.g., which split reduces abstraction size least)

### 7. Derived vs Regular Numeric Variables

**Important distinction**:

- **Regular numeric variables**: Explicitly part of the abstract state, have explicit `NumericDomainMapping` with partitions
- **Derived numeric variables** (from assignment axioms): NOT part of abstract state, partitions are implicitly defined by the ranges of their source variables

**Refinement only applies to regular numeric variables**:
```cpp
// Only refine regular numeric variables
void refine_numeric_variable(int numeric_var_id, ap_float value) {
    // Check if this is a regular numeric variable (not derived)
    if (numeric_var_id < numeric_domain_mapping.size()) {
        numeric_domain_mapping[numeric_var_id].split_at(value);
        numeric_domain_sizes[numeric_var_id] = 
            numeric_domain_mapping[numeric_var_id].get_num_partitions();
    }
    // If it's a derived variable, we don't refine it directly
    // Its partitions change implicitly when we refine its source variables
}
```

**Why this works**:
- Derived numeric variables are computed from regular numeric variables
- Example: `derived := x + y`
  - If `x ∈ [0, 10)` and `y ∈ [5, 15)`, then `derived ∈ [5, 25)`
  - If we split `x` at 5, we get `x ∈ [0, 5)` or `x ∈ [5, 10)`
  - This automatically refines `derived` into `[5, 20)` and `[10, 25)`
- Comparison axioms on derived variables will use these implicit partitions

**In flaw detection**:
```cpp
if (is_comparison_axiom(pre.get_variable())) {
    ComparisonAxiomInfo info = comparison_axiom_map[prop_var];
    int numeric_var_id = info.numeric_var_id;
    
    // This could be a regular OR derived numeric variable
    // We only refine if it's regular
    if (is_regular_numeric_variable(numeric_var_id)) {
        ap_float concrete_value = numeric_state[numeric_var_id];
        refinements.push_back({numeric_var_id, concrete_value, ...});
    } else {
        // Derived variable - need to trace back to source variables
        // (more complex, might want to skip or handle specially)
    }
}
```

### 8. Integration Points

**Files to modify**:

1. **`cegar.cc`**:
   - Add `vector<NumericDomainMappingType> numeric_domain_mapping` as member
   - Add `vector<int> numeric_domain_sizes` as member
   - Add `unordered_set<int> blacklisted_numeric_variables` as member
   - Modify `get_flaws()` to detect numeric flaws
   - Add `fix_numeric_flaws()` method
   - Modify CEGAR loop to handle numeric refinements

2. **`domain_abstraction_factory.cc`**:
   - Already supports numeric variables ✓
   - Ensure plan extraction handles numeric state correctly

3. **New helper class** (optional):
   ```cpp
   class NumericFlawHandler {
       // Map comparison axioms to numeric conditions
       // Extract threshold values
       // Determine which numeric variables to refine
   };
   ```

---

## Key Insights and Design Decisions

### 1. Split at Concrete Value, Not Threshold

**Critical realization**: When a numeric precondition fails, we must split at the **concrete value** that caused the flaw, not the threshold from the comparison.

**Example that shows why threshold-only doesn't work**:
```
Flaw: fuel ≥ 50 required, but concrete state has fuel = 30
Wrong: Split at 50 → partitions: (-∞, 50), [50, ∞)
  Problem: fuel=30 still in (-∞, 50) with fuel=49
  Result: Abstraction still can't distinguish them!

Right: Split at 30 → partitions: (-∞, 30), [30, ∞)
  Progress: fuel=30 now in [30, ∞), separated from fuel<30
  Next iteration might split at 40, then 50, etc.
  Progressive refinement converges!
```

**Alternative (aggressive)**: Split at BOTH concrete value (30) AND threshold (50)
- Creates: (-∞, 30), [30, 50), [50, ∞)
- Faster convergence but larger abstract state space

### 2. Derived Numeric Variables Don't Need Refinement

**Important**: Only refine **regular numeric variables**. Derived numeric variables (from assignment axioms) have partitions that are implicitly defined by their source variables.

**Reason**: If `derived := x + y`, and we refine `x` or `y`, the partitions of `derived` automatically change. No explicit refinement needed!

### 3. Progressive vs Aggressive Refinement

**Progressive** (split at concrete value only):
- Smaller refinement steps
- More CEGAR iterations
- Better for staying within abstraction size limit

**Aggressive** (split at both concrete value and threshold):
- Larger refinement steps
- Fewer CEGAR iterations
- Risk of exceeding abstraction size limit sooner

## Summary

### Current Status
✅ Numeric variables are initialized with full range `(-∞, ∞)`  
✅ NumericDomainMapping supports splitting at threshold values  
✅ Comparison axioms compile numeric conditions to propositional variables  
✅ Abstract operator construction handles numeric variables  

### Remaining Work
❌ Flaw detection for numeric preconditions  
❌ Flaw detection for numeric goals  
❌ Mapping from comparison axioms back to numeric variables/thresholds  
❌ Refinement logic for numeric variables (call `split_at()`)  
❌ Plan execution with numeric state tracking  
❌ Size limit checking for numeric variable refinement  

### Recommended Next Steps

1. **Build comparison axiom map**: Create mapping from derived propositional variables to underlying numeric conditions
2. **Extend flaw detection**: Modify `get_flaws()` to track numeric state and detect comparison axiom flaws
3. **Implement numeric refinement**: Add `fix_numeric_flaws()` method that calls `split_at()` on numeric domain mappings
4. **Test incrementally**: Start with simple domains (one numeric variable, one comparison)
5. **Handle edge cases**: Multiple flaws, size limits, blacklisting

The hybrid approach (Solution 3) leverages existing infrastructure while cleanly extending to numeric variables.
