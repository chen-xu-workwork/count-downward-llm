import re
import sys
from pathlib import Path
from collections import defaultdict, deque

DEBUG_PRINT = False

input_file_path = sys.argv[1]
# Assuming 'sas_output' holds the content of your SAS output
with open(input_file_path, "r") as f:
    sas_output = f.read()  # Replace with your actual SAS output

# 1. Extract Variables
variables = {}
for var_match in re.finditer(
        r"begin_variable\s+(var\d+)\s+(.*?\d+)\s+(\d+)\s+(.*?)\s+end_variable",
        sas_output,
        re.DOTALL,
):
    var_name, num1, num2, values = var_match.groups()

    #print(var_name)
    variables[var_name] = {
        "num1": int(num1),
        "num2": int(num2),
        "values": [v.strip() for v in values.splitlines()],
    }

#print(variables)

# 2. Extract Numeric Variables
numeric_vars = []
num_vars_match = re.search(
    r"begin_numeric_variables\s+(.*?)\s+end_numeric_variables",
    sas_output,
    re.DOTALL,
)
if num_vars_match:
    numeric_vars = [
        v.strip() for v in num_vars_match.group(1).splitlines() if v.strip()
    ]

# 3. Extract Rules
rules = []
for rule_match in re.finditer(
        r"begin_rule\s+(.*?)\s+end_rule", sas_output, re.DOTALL
):
    rules.append([v.strip() for v in rule_match.group(1).splitlines()])

# 4. Extract Axioms
axioms = {}
axioms_match = re.search(
    r"begin_comparison_axioms\s+(.*?)\s+end_comparison_axioms",
    sas_output,
    re.DOTALL,
)
if axioms_match:
    axioms["comparison"] = [
        v.strip() for v in axioms_match.group(1).splitlines()
    ]

axioms_match = re.search(
    r"begin_numeric_axioms\s+(.*?)\s+end_numeric_axioms",
    sas_output,
    re.DOTALL,
)
if axioms_match:
    axioms["numeric"] = [v.strip() for v in axioms_match.group(1).splitlines()]

initial_state = []
initial_state_match = re.search(
    r"begin_state\s+(.*?)\s+end_state", sas_output, re.DOTALL
)
if initial_state_match:
    initial_state = [
        int(v.strip())
        for v in initial_state_match.group(1).splitlines()
        if v.strip()
    ]

# 6. Extract Initial State (Numeric Variables)
initial_numeric_state = []
initial_numeric_state_match = re.search(
    r"begin_numeric_state\s+(.*?)\s+end_numeric_state", sas_output, re.DOTALL
)
if initial_numeric_state_match:
    initial_numeric_state = [
        float(v.strip())
        for v in initial_numeric_state_match.group(1).splitlines()
        if v.strip()
    ]


# Print or process the extracted information as needed
def print_all():
    print("Variables:", variables)
    print("Numeric Variables:", numeric_vars)
    print("Rules:", rules)
    print("Axioms:", axioms)
    print("Initial State:", initial_state)
    print("Initial Numeric State:", initial_numeric_state)
    print("Real numeric variables", real_numeric_variables)
    print("Operators:", operators)


#print_all()

def get_var_value(var_number):
    return initial_numeric_state[var_number]


def is_real_variable(var_number):
    return numeric_vars[var_number][0] == 'R'


real_numeric_variables = []
for i in range(len(numeric_vars)):
    if is_real_variable(i):
        real_numeric_variables.append(i)

# Snapshot of the "base" real variables that existed in the original SAS.
# Variables turned into real later (e.g., comparison-LHS realizations) are not in this set.
original_real_numeric_variables = set(real_numeric_variables)

# Map numeric variable id -> position in real_numeric_variables for O(1) lookup.
real_var_pos = {var_id: pos for pos, var_id in enumerate(real_numeric_variables)}


def parse_operators(sas_output):
    """
    Parses operators from SAS output in the Fast Downward translator format.

    Args:
      sas_output: The SAS output string.

    Returns:
      A list of dictionaries, where each dictionary represents an operator
      with its properties.
    """
    operators = []
    for op_match in re.finditer(
            r"begin_operator\s+(.*?)\s+end_operator", sas_output, re.DOTALL
    ):
        op_lines = [v.strip() for v in op_match.group(1).splitlines()]

        # Extract common operator properties
        operator = {
            "name": op_lines[0],
            "num_preconditions": int(op_lines[1]),
            "num_ass_effects": 0,
            "num_effects": 1,
            "cost": float(op_lines[-1]),  # Assuming cost is always a float
        }

        # Extract preconditions and effects
        precondition_start = 2  # Index where preconditions start
        preconditions = []
        for i in range(operator["num_preconditions"]):
            preconditions.append(op_lines[precondition_start + i])
        operator["preconditions"] = preconditions

        operator["num_ass_effects"] = int(op_lines[precondition_start + operator["num_preconditions"]])

        operator['num_effects'] = int(op_lines[precondition_start + operator["num_preconditions"] + 1 + operator['num_ass_effects']])
        ass_eff = []
        ass_eff_start = precondition_start + operator["num_preconditions"] + 1
        for i in range(operator['num_ass_effects']):
            ass_eff.append(op_lines[ass_eff_start + i])

        operator['ass_effects'] = ass_eff
        effects_start = ass_eff_start + operator['num_ass_effects'] + 1
        effects = []
        for i in range(operator["num_effects"]):
            effects.append(op_lines[effects_start + i])
        operator["effects"] = effects

        operators.append(operator)

    return operators


# Assuming 'sas_output' holds the content of your SAS output
# (Replace this with your actual SAS output)

operators = parse_operators(sas_output)


def update_var(var, upd_var_number, is_update_multiplication):
    if is_real_variable(upd_var_number):
        print("Trying to operate on multiple real variables outside of formula!")
        return
    upd_val = get_var_value(upd_var_number)
    # correct initial value
    # correct effect value
    # don't touch axioms
    if not is_update_multiplication:
        initial_numeric_state[var] += upd_val
        return

    initial_numeric_state[var] *= upd_val

    for operator in operators:
        for i in range(len(operator['effects'])):
            vals = operator['effects'][i].split(" ")
            var1 = int(vals[1])
            var2 = int(vals[-1])

            if is_real_variable(var1) and is_real_variable(var2):
                print("Ty dolboeb? Kak ti skladivayesh dve realnie peremennie?")
                return

            if var1 == var:
                numeric_vars.append(
                    "C " + str(len(numeric_vars)) + " PNE derived! " + str(var2) + " * " + str(upd_var_number))
                initial_numeric_state.append(get_var_value(var2) * upd_val)
                var2 = len(initial_numeric_state) - 1

                operator['effects'][i] = vals[0] + " " + str(var1) + " " + vals[2] + " " + str(var2)


def is_var_useful(var):
    # On large instances, repeated scans over all axioms/effects dominate.
    # If we have a precomputed set of referenced numeric vars, use it.
    global used_numeric_vars
    try:
        return 1 if int(var) in used_numeric_vars else 0
    except Exception:
        pass

    for i in range(len(axioms['comparison'])):
        vals = axioms['comparison'][i].split()

        for val in range(1, len(vals)):
            if vals[val] == str(var):
                return 1

    for i in range(len(axioms['numeric'])):
        vals = axioms['numeric'][i].split()
        for val in range(1, len(vals)):
            if vals[val] == str(var):
                return 1

    for operator in operators:
        for i in range(len(operator['effects'])):
            vals = operator['effects'][i].split(" ")
            var11 = int(vals[1])
            var22 = int(vals[-1])
            if(int(var) == var11):
                return 1
            if(int(var) == var22):
                return 1
    return 0

def replace_var(var1, var2):
    for i in range(len(axioms['comparison'])):
        vals = axioms['comparison'][i].split()

        for val in range(1, len(vals)):
            if vals[val] == str(var1):
                if DEBUG_PRINT:
                    print(vals, var1, var2)
                vals[val] = str(var2)
        #print(vals)
        axioms['comparison'][i] = " ".join(vals)

    for i in range(len(axioms['numeric'])):
        vals = axioms['numeric'][i].split()
        for val in range(1, len(vals)):
            if vals[val] == str(var1):
                vals[val] = str(var2)
        #print(vals)
        axioms['numeric'][i] = " ".join(vals)

    for operator in operators:
        for i in range(len(operator['effects'])):
            vals = operator['effects'][i].split(" ")
            var11 = int(vals[1])
            var22 = int(vals[-1])
            flag = 0
            if(int(var1) == var11):
                flag = 1
                var11 = var2
            if(int(var1) == var22):
                flag = 1
                var22 = var2

            operator['effects'][i] = vals[0] + " " + str(var11) + " " + vals[2] + " " + str(var22)
            if(flag):
                if DEBUG_PRINT:
                    print("Change: ", var1, var2, vals, operator['effects'][i])
                #print(operator, i)


#We want to make a formula for each numeric var according to numeric axiom tree
#We store the formula in the following way:
#[(c0, const), (c1, rv1), (c2, rv2), ... (cn, real_variable_n)]
#for +/- we simply add coefficents for each variable respectively
#for * we search for non-zero coef near any variable and multiply this formula by coef of the other one
#there can be only 1 formula out of the 2 with non-zero coef near rv in it, since we oly work with linears here
#ROOM FOR IMPROVEMENT: we maybe can re-use formulas

formulas = {}

# Real vars that occur together with at least one other real var in some
# *linear computation* (affine formula or additive numeric effect).
# If such a variable is affected by an assignment-like numeric effect, we
# currently refuse to compile it (see user rule).
mixed_real_vars_in_linear_computation = set()


def add_or_minus_formulas(f1, f2, op):
    f3 = [0] * len(f1)
    if op == "+":
        for i in range(len(f1)):
            f3[i] = f1[i] + f2[i]
    else:
        for i in range(len(f1)):
            f3[i] = f1[i] - f2[i]
    return f3


def mult_formula(f1, f2, op):
    f3 = [0] * len(f1)
    flag = 1 if any(x != 0 for x in f1[1:]) else 0
    if (flag):
        for i in range(len(f1)):
            f3[i] = f1[i] * f2[0]
    else:
        for i in range(len(f1)):
            f3[i] = f1[0] * f2[i]

    return f3


def operate(f1, f2, op):
    if op == "*":
        return mult_formula(f1, f2, op)
    else:
        return add_or_minus_formulas(f1, f2, op)


def gen_initial_formula(var):
    f = [0] * (len(real_numeric_variables) + 1)
    if is_real_variable(var):
        pos = real_var_pos.get(var)
        if pos is None:
            print("UEBISHE LESNOE")
        else:
            f[pos + 1] = 1
    else:
        f[0] = get_var_value(var)
    return f


def gen_all_formulas_for_all_vars_met_in_axiom(axiom):
    var_final = int(axiom.split()[0])
    op = axiom.split()[1]
    var1 = int(axiom.split()[2])
    var2 = int(axiom.split()[3])

    f1 = []
    if var1 in formulas.keys():
        f1 = formulas[var1]
    else:
        f1 = gen_initial_formula(var1)
        formulas[var1] = f1
    f2 = []
    if var2 in formulas.keys():
        f2 = formulas[var2]
    else:
        f2 = gen_initial_formula(var2)
        formulas[var2] = f2

    f3 = operate(f1, f2, op)
    #print(var_final, f3, var1, f1, var2, f2)
    formulas[var_final] = f3


def is_var_final_in_axiom(var):
    for i in range(len(axioms['numeric'])):
        axiom = axioms['numeric'][i]
        var_final = int(axiom.split()[0])
        if (var == var_final):
            return i
    return -1


"""
def gen_all_formulas():
    axiom_que = []
    axioms_visited = [False] * len(axioms['numeric'])
    axioms_dist = [0] * len(axioms['numeric'])

    for i in range(len(axioms['numeric'])):
        if axioms_visited[i]:
            continue

        local_que = [i]
        while local_que:
            t = local_que.pop(0)  # Use pop(0) for efficiency
            if axioms_visited[t]:
                continue

            axiom = axioms['numeric'][t]
            axioms_visited[t] = True
            var_final = int(axiom.split()[0])
            op = axiom.split()[1]
            var1 = int(axiom.split()[2])
            var2 = int(axiom.split()[3])

            for var, dist_offset in [(var1, 1), (var2, 1)]:  # Check var1 and var2
                try:
                    idx = is_var_final_in_axiom(var)
                    if idx == -1:
                        continue
                    if not axioms_visited[idx]:
                        local_que.append(idx)
                        axioms_dist[idx] = axioms_dist[t] + dist_offset
                except ValueError:
                    pass

    axiom_que = sorted(range(len(axioms['numeric'])), key=lambda i: axioms_dist[i])
    print(axiom_que)
    for i in range(len(axiom_que)):
        gen_all_formulas_for_all_vars_met_in_axiom(axioms['numeric'][axiom_que[-i-1]])

        #gen_all_formulas_for_all_vars_met_in_axiom(axioms['numeric'][-i-1])

gen_all_formulas()
"""
def topological_sort_predecessors(predecessors_list):
    """
    Performs a topological sort given a list of predecessors for each node.

    Args:
      predecessors_list: A dictionary where keys are nodes and values are lists
                          of their predecessors.

    Returns:
      A list of nodes in topological order, or None if the graph
      contains a cycle.
    """

    graph = defaultdict(list)  # Create the graph in the usual format
    in_degree = defaultdict(int)

    for node in predecessors_list:
        for predecessor in predecessors_list[node]:
            graph[predecessor].append(node)  # Reverse the edges
            in_degree[node] += 1

    queue = deque([node for node in predecessors_list if in_degree[node] == 0])
    result = []

    #print(graph)
    #print(queue)
    while queue:
        node = queue.popleft()
        result.append(node)

        for neighbor in graph[node]:
            in_degree[neighbor] -= 1
            if in_degree[neighbor] == 0:
                queue.append(neighbor)

    if len(result) != len(predecessors_list):  # Cycle detected
        return None
    else:
        return result


# Example usage:
predecessors_list = {}

for axiom in axioms['numeric']:
    var_final = int(axiom.split()[0])
    op = axiom.split()[1]
    var1 = int(axiom.split()[2])
    var2 = int(axiom.split()[3])

    predecessors_list[var_final] = [var1, var2]

for i in range(len(numeric_vars)):
    if i not in predecessors_list.keys():
        predecessors_list[i] = []

sorted_nodes = topological_sort_predecessors(predecessors_list)

"""
if sorted_nodes:
    print("Topological Sort:", sorted_nodes)
else:
    print("The graph contains a cycle.")
"""


numeric_axiom_by_final = {}
for axiom in axioms.get('numeric', []):
    parts = axiom.split()
    if not parts:
        continue
    try:
        numeric_axiom_by_final[int(parts[0])] = axiom
    except ValueError:
        continue

for node in sorted_nodes or []:
    axiom = numeric_axiom_by_final.get(node)
    if axiom is None:
        continue
    gen_all_formulas_for_all_vars_met_in_axiom(axiom)

added_constants = {} #val : idx

def update_var_with_formula(var):
    if (var in formulas.keys()):
        formula = formulas[var]
    else:
        formula = gen_initial_formula(var)
        formulas[var] = formula
    total_initial_upd_value = 0
    for i in range(1, len(formula)):
        #if(var == 9):
        #    print(formula[i], real_numeric_variables[i - 1], get_var_value(real_numeric_variables[i - 1]))
        total_initial_upd_value += formula[i] * get_var_value(real_numeric_variables[i - 1])

    # For newly introduced real variables representing an affine expression,
    # their value must include the constant term (formula[0]) plus the
    # contributions from the base real variables.
    initial_numeric_state[var] = formula[0] + total_initial_upd_value

    for operator in operators:
        total_effect_upd_value = 0
        for effect in operator['effects']:

            var1 = int(effect.split()[1])
            var2 = int(effect.split()[3])
            op = effect.split()[2]
            #if(var == 2):
            #    print(effect, var1, (var1 in real_numeric_variables), op)
            pos = real_var_pos.get(var1)
            if pos is None:
                continue
            if (op == "+"):
                total_effect_upd_value += formula[pos + 1] * get_var_value(var2)
            elif op == "-":
                total_effect_upd_value -= formula[pos + 1] * get_var_value(var2)
            else:
                # Assignment-like update (e.g., :=, *=, etc.). We can ignore it
                # for the current compiled linear delta if the current formula
                # does not depend on this variable.
                coeff = formula[pos + 1]
                if coeff == 0:
                    continue

                # If the assigned variable is used in any other linear
                # computation together with another real variable, we fail
                # fast (requested behavior).
                if var1 in mixed_real_vars_in_linear_computation:
                    # Hard failure: this is a "wrong" assignment effect that we
                    # must not silently keep. Exit with code 33 so callers can
                    # distinguish this case from other errors.
                    msg = (
                        "Unsupported assignment-like numeric effect: variable "
                        f"{var1} participates in a linear computation with another real variable. "
                        f"Offending effect: '{effect}' in operator '{operator.get('name')}'."
                    )
                    sys.stderr.write(msg + "\n")
                    sys.exit(33)

                # Otherwise, leave the effect as-is and do not try to derive an
                # additional linear delta for the current compiled variable.
                continue
        if (total_effect_upd_value != 0):
            operator['num_effects'] += 1
            add_idx = len(numeric_vars)
            if total_effect_upd_value in added_constants.keys():
                add_idx = added_constants[total_effect_upd_value]
                operator['effects'].append("0 " + str(var) + " + " + str(add_idx))
            else:
                numeric_vars.append(
                    "C -1 " + "!derived" + str(total_effect_upd_value) + "from" + str(var) + " : " + str(formula))
                initial_numeric_state.append(total_effect_upd_value)
                operator['effects'].append("0 " + str(var) + " + " + str(add_idx))
                added_constants[total_effect_upd_value] = add_idx


for i in range(len(axioms['comparison'])):
    axiom = axioms['comparison'][i]
    var1 = int(axiom.split()[2])
    var2 = int(axiom.split()[-1])
    numeric_vars[var1] = "R" + numeric_vars[var1][1:]
    if var1 not in formulas.keys():
        formula = gen_initial_formula(var1)
        formulas[var1] = formula

    # Keep the RHS unchanged. The LHS variable will be updated to represent
    # the full affine value (including the constant term), so shifting the RHS
    # would be incorrect.


def _compute_mixed_real_vars_in_linear_computation() -> set:
    """Return real numeric vars that co-occur with another real var in a linear computation.

    We conservatively mark a real variable as "mixed" if:
      (1) it appears with at least one other real variable in any affine formula,
          i.e., any stored formula has >= 2 non-zero real coefficients; OR
      (2) it appears in an additive numeric effect together with another real var
          (target and rhs both real) via '+' or '-'.
    """
    mixed = set()

    # (1) Mixed in affine formulas.
    for formula in formulas.values():
        if not formula:
            continue
        nonzero_reals = []
        # formula[1:] aligns with real_numeric_variables via real_var_pos.
        for i, coeff in enumerate(formula[1:], start=0):
            if coeff != 0:
                if i < len(real_numeric_variables):
                    nonzero_reals.append(real_numeric_variables[i])
        if len(nonzero_reals) >= 2:
            mixed.update(nonzero_reals)

    # (2) Mixed directly in operator numeric effects.
    for op in operators:
        for eff in op.get('effects', []):
            parts = eff.split()
            if len(parts) < 4:
                continue
            try:
                target = int(parts[1])
                rhs = int(parts[3])
            except ValueError:
                continue
            oper = parts[2]
            if oper in ('+', '-') and is_real_variable(target) and is_real_variable(rhs):
                mixed.add(target)
                mixed.add(rhs)

    return mixed


def _validate_assignment_like_numeric_effects():
    """Validate assignment-like numeric effects according to the requested rule.

    Rule: if a numeric effect is assignment-like (i.e., not '+' or '-') and its
    affected variable participates in any other linear computation with at least
    one other real variable, throw; otherwise leave the effect as-is.
    """
    for op in operators:
        for eff in op.get('effects', []):
            parts = eff.split()
            if len(parts) < 4:
                continue
            oper = parts[2]
            if oper in ('+', '-'):
                continue
            try:
                target = int(parts[1])
            except ValueError:
                continue

            # Only meaningful for real variables (the rule talks about "other real variables").
            if not is_real_variable(target):
                continue

            if target in mixed_real_vars_in_linear_computation:
                # Hard failure: this is a "wrong" assignment effect that we
                # must not silently keep. Exit with code 33 so callers can
                # distinguish this case from other errors.
                msg = (
                    "Unsupported assignment-like numeric effect: affected variable "
                    f"{target} participates in a linear computation with another real variable. "
                    f"Offending effect: '{eff}' in operator '{op.get('name')}'."
                )
                sys.stderr.write(msg + "\n")
                sys.exit(33)
#print_all()

# After formulas are known and all comparison LHS vars have been marked as real,
# compute which real vars are "mixed" and validate assignment-like effects.
mixed_real_vars_in_linear_computation = _compute_mixed_real_vars_in_linear_computation()
_validate_assignment_like_numeric_effects()

for i in range(len(numeric_vars)):
    if is_real_variable(i) and i not in real_numeric_variables:
        update_var_with_formula(i)


# From here on, numeric axioms are no longer part of the task semantics: their
# effect has been compiled into numeric effects. Keeping them around would
# incorrectly prevent dead-variable elimination.
axioms['numeric'] = []


# 10. Goal
goal_match = re.search(r"begin_goal\s+(.*?)\s+end_goal", sas_output, re.DOTALL)
if goal_match:
    goal = [v.strip() for v in goal_match.group(1).splitlines()]

# 11. Global Constraints
global_constraint_match = re.search(
    r"begin_global_constraint\s+(.*?)\s+end_global_constraint",
    sas_output,
    re.DOTALL,
)
if global_constraint_match:
    global_constraints = [
        v.strip() for v in global_constraint_match.group(1).splitlines()
    ]
# 1. Version
version_match = re.search(r"begin_version\s+(\d+)\s+end_version", sas_output)
if version_match:
    version = int(version_match.group(1))

# 2. Metric
metric_match = re.search(r"begin_metric\s+(.*?)\s+end_metric", sas_output, re.DOTALL)
if metric_match:
    metric = metric_match.group(1).strip()
else:
    metric = "0 0"

metric_tokens = metric.split()
metric_criterion = metric_tokens[0] if len(metric_tokens) >= 1 else "0"
try:
    metric_index = int(metric_tokens[1]) if len(metric_tokens) >= 2 else 0
except ValueError:
    metric_index = 0

print("OUR FINAL METRIC IS ", metric_criterion, metric_index)


def build_used_numeric_vars():
    used = set()
    for line in axioms.get('comparison', []):
        parts = line.split()
        if len(parts) != 4:
            continue
        # parts[0] is a propositional variable id.
        for pos in (2, 3):
            try:
                used.add(int(parts[pos]))
            except ValueError:
                pass

    for line in axioms.get('numeric', []):
        parts = line.split()
        if len(parts) != 4:
            continue
        for pos in (0, 2, 3):
            try:
                used.add(int(parts[pos]))
            except ValueError:
                pass

    for op in operators:
        for eff in op.get('effects', []):
            parts = eff.split()
            if len(parts) < 4:
                continue
            try:
                used.add(int(parts[1]))
                used.add(int(parts[3]))
            except ValueError:
                pass

    return used


used_numeric_vars = build_used_numeric_vars()


def compute_used_propositional_vars():
    used = set()

    # Goals are propositional facts: "<var> <value>".
    for g in goal if 'goal' in globals() else []:
        parts = g.split()
        if not parts:
            continue
        try:
            used.add(int(parts[0]))
        except ValueError:
            pass

    # Operator preconditions are propositional facts: "<var> <value>".
    for op in operators:
        for pr in op.get('preconditions', []):
            parts = pr.split()
            if not parts:
                continue
            try:
                used.add(int(parts[0]))
            except ValueError:
                pass

    # Rules contain propositional conditions and a single propositional effect.
    # Format written by this script:
    #   begin_rule
    #   <n_conditions>
    #   <var> <value>   (repeated n_conditions times)
    #   <var> <value>   (effect)
    #   end_rule
    for rule in rules:
        if not rule:
            continue
        try:
            n_conds = int(rule[0])
        except (ValueError, TypeError):
            continue

        cond_lines = rule[1:1 + n_conds]
        eff_line = rule[1 + n_conds] if 1 + n_conds < len(rule) else None

        for line in cond_lines:
            parts = line.split()
            if not parts:
                continue
            try:
                used.add(int(parts[0]))
            except ValueError:
                pass

        if eff_line:
            parts = eff_line.split()
            if parts:
                try:
                    used.add(int(parts[0]))
                except ValueError:
                    pass

    return used


def filter_comparison_axioms_to_relevant_props(used_prop_vars):
    filtered = []
    for line in axioms.get('comparison', []):
        parts = line.split()
        if len(parts) != 4:
            filtered.append(line)
            continue
        try:
            prop_var = int(parts[0])
        except ValueError:
            filtered.append(line)
            continue
        if prop_var in used_prop_vars:
            filtered.append(line)
    axioms['comparison'] = filtered


def prune_irrelevant_numeric_variables():
    """Remove numeric variables not relevant for any used propositional facts.

    We keep numeric variables that are needed to evaluate comparison axioms whose
    propositional result is used in goals, rules, or operator preconditions.
    Additionally, we keep the metric variable (if any) and close over numeric
    dependencies introduced by numeric operator effects.
    """

    global numeric_vars, initial_numeric_state, metric_index

    used_props = compute_used_propositional_vars()
    filter_comparison_axioms_to_relevant_props(used_props)

    required = set()

    # Always keep the metric variable.
    if 0 <= metric_index < len(numeric_vars):
        required.add(metric_index)

    # Seed from relevant comparison axioms.
    for line in axioms.get('comparison', []):
        parts = line.split()
        if len(parts) != 4:
            continue
        for pos in (2, 3):
            try:
                required.add(int(parts[pos]))
            except ValueError:
                pass

    # Close over dependencies via numeric effects: if we need to maintain a
    # variable, we also need the variables used on the RHS of its updates.
    changed = True
    while changed:
        changed = False
        for op in operators:
            for eff in op.get('effects', []):
                parts = eff.split()
                if len(parts) < 4:
                    continue
                try:
                    target = int(parts[1])
                    rhs = int(parts[3])
                except ValueError:
                    continue
                if target in required and rhs not in required:
                    required.add(rhs)
                    changed = True

    if not required:
        # Should never happen since metric_index is always required, but guard anyway.
        return

    kept = sorted(v for v in required if 0 <= v < len(numeric_vars))
    old_to_new = {old: new for new, old in enumerate(kept)}

    def remap_effects(effects):
        new_effects = []
        for line in effects:
            parts = line.split()
            if len(parts) < 4:
                new_effects.append(line)
                continue
            try:
                target = int(parts[1])
                rhs = int(parts[3])
            except ValueError:
                new_effects.append(line)
                continue
            # Drop updates to numeric vars that are irrelevant.
            if target not in old_to_new:
                continue
            if rhs not in old_to_new:
                # If this happens, the closure logic above missed a dependency;
                # conservatively keep it by not dropping but also not remapping.
                new_effects.append(line)
                continue
            parts[1] = str(old_to_new[target])
            parts[3] = str(old_to_new[rhs])
            new_effects.append(" ".join(parts))
        return new_effects

    def remap_comparison(line: str) -> str:
        parts = line.split()
        if len(parts) != 4:
            return line
        try:
            v1 = int(parts[2])
            v2 = int(parts[3])
        except ValueError:
            return line
        if v1 in old_to_new:
            parts[2] = str(old_to_new[v1])
        if v2 in old_to_new:
            parts[3] = str(old_to_new[v2])
        return " ".join(parts)

    def remap_numeric_axiom(line: str) -> str:
        parts = line.split()
        if len(parts) != 4:
            return line
        for pos in (0, 2, 3):
            try:
                v = int(parts[pos])
            except ValueError:
                continue
            if v in old_to_new:
                parts[pos] = str(old_to_new[v])
        return " ".join(parts)

    axioms['comparison'] = [remap_comparison(a) for a in axioms.get('comparison', [])]
    axioms['numeric'] = [remap_numeric_axiom(a) for a in axioms.get('numeric', [])]

    for op in operators:
        op['effects'] = remap_effects(op.get('effects', []))
        # op['ass_effects'] are propositional pre_post effects; keep them as-is.

        op['effects'] = list(dict.fromkeys(op['effects']))
        op['num_effects'] = len(op['effects'])
        op['ass_effects'] = list(dict.fromkeys(op['ass_effects']))
        op['num_ass_effects'] = len(op['ass_effects'])

    numeric_vars = [numeric_vars[i] for i in kept]
    initial_numeric_state = [initial_numeric_state[i] for i in kept]
    if metric_index in old_to_new:
        metric_index = old_to_new[metric_index]

formula_to_var = {}
def duplicate_detect_formulas():
    old_num_vars = len(numeric_vars)
    useful = [bool(is_var_useful(i)) for i in range(old_num_vars)]

    # The metric variable must never be removed; the preprocessor expects a
    # valid integer index here.
    if 0 <= metric_index < old_num_vars:
        useful[metric_index] = True
    total_useful = sum(1 for u in useful if u)

    # For each numeric var index, decide whether to keep it, and if not kept,
    # which kept index it should be mapped to (for duplicates).
    rep_for_formula = {}
    old_to_rep = {}
    removed = set()

    for var in range(old_num_vars):
        if var == metric_index:
            old_to_rep[var] = var
            continue
        # Keep original (base) real variables as-is.
        if var in original_real_numeric_variables:
            old_to_rep[var] = var
            continue

        # For newly-real variables (typically realized comparison LHS vars),
        # we can still deduplicate them if their affine formulas are identical.
        is_new_real = is_real_variable(var)
        if not useful[var]:
            removed.add(var)
            continue

        formula = formulas.get(var)
        if formula is None:
            # Fallback: keep unique (don't merge) if formula is missing.
            # This avoids accidentally merging variables that should differ.
            key = ("NOFORMULA", var)
        else:
            # Use an immutable key and keep type separate so we never merge
            # across real/non-real variable categories.
            key = ("R" if is_new_real else "NR", tuple(formula))
        if key in rep_for_formula:
            rep = rep_for_formula[key]
            old_to_rep[var] = rep
            removed.add(var)
        else:
            rep_for_formula[key] = var
            old_to_rep[var] = var

    kept = [i for i in range(old_num_vars) if i not in removed]
    old_to_new = {old: new for new, old in enumerate(kept)}

    # Final mapping for all old indices that must be rewritten.
    index_map = {}
    for old_idx in range(old_num_vars):
        if old_idx in old_to_rep:
            rep = old_to_rep[old_idx]
            if rep in old_to_new:
                index_map[old_idx] = old_to_new[rep]

    def remap_effect_line(line: str) -> str:
        parts = line.split()
        # expected: <cond-count> <numvar> <op> <numvar>
        if len(parts) >= 4:
            try:
                v1 = int(parts[1])
                v2 = int(parts[3])
            except ValueError:
                return line
            if v1 in index_map:
                parts[1] = str(index_map[v1])
            if v2 in index_map:
                parts[3] = str(index_map[v2])
            return " ".join(parts)
        return line

    def remap_comparison_axiom(line: str) -> str:
        parts = line.split()
        if len(parts) != 4:
            return line
        try:
            v1 = int(parts[2])
            v2 = int(parts[3])
        except ValueError:
            return line
        if v1 in index_map:
            parts[2] = str(index_map[v1])
        if v2 in index_map:
            parts[3] = str(index_map[v2])
        return " ".join(parts)

    def remap_numeric_axiom(line: str) -> str:
        parts = line.split()
        if len(parts) != 4:
            return line
        for pos in (0, 2, 3):
            try:
                v = int(parts[pos])
            except ValueError:
                continue
            if v in index_map:
                parts[pos] = str(index_map[v])
        return " ".join(parts)

    # Apply remapping.
    axioms['comparison'] = [remap_comparison_axiom(a) for a in axioms.get('comparison', [])]
    axioms['numeric'] = [remap_numeric_axiom(a) for a in axioms.get('numeric', [])]
    for op in operators:
        op['effects'] = [remap_effect_line(e) for e in op.get('effects', [])]
        # op['ass_effects'] are propositional pre_post effects; they do not
        # reference numeric variables and must not be remapped here.

    # Rebuild numeric variables and numeric state.
    new_numeric_vars = [numeric_vars[i] for i in kept]
    new_initial_numeric_state = [initial_numeric_state[i] for i in kept]

    if DEBUG_PRINT:
        print(len(new_numeric_vars), len(removed), total_useful)

    return new_numeric_vars, new_initial_numeric_state, index_map


def get_i_var():
    for i in range(len(numeric_vars)):
        if(numeric_vars[i][0] == "I"):
            return i

#duplicate_detect_formulas()
numeric_vars, initial_numeric_state, index_map = duplicate_detect_formulas()
metric_index = index_map.get(metric_index, metric_index)

for operator in operators:
    operator['effects'] = list(dict.fromkeys(operator['effects']))
    operator['num_effects'] = len(operator['effects'])
    operator['ass_effects'] = list(dict.fromkeys(operator['ass_effects']))
    operator['num_ass_effects'] = len(operator['ass_effects'])


# Final cleanup: drop numeric vars that are not relevant to any used
# comparison axioms (i.e., comparisons whose propositional result is not used
# in goals/rules/operator preconditions), and drop their associated updates.
prune_irrelevant_numeric_variables()


def print_result(output_file):
    with open(output_file, "w") as out:
        print("begin_version", file=out)
        print(version, file=out)
        print("end_version", file=out)
        print("begin_metric", file=out)
        print(metric_criterion, metric_index, file=out)
        print("end_metric", file=out)
        print(len(variables), file=out)
        for varname in variables.keys():
            print("begin_variable", file=out)
            print(varname, file=out)
            print(variables[varname]['num1'], file=out)
            #print(len(numeric_vars) + (varname == "var0" or varname == "var1"))
            print(variables[varname]['num2'], file=out)
            for vl in variables[varname]['values']:
                print(vl, file=out)
            print("end_variable", file=out)

        print(len(numeric_vars), file=out)
        print("begin_numeric_variables", file=out)
        for i in numeric_vars:
            print(i, file=out)
        print("end_numeric_variables", file=out)
        print(0, file=out)
        print("begin_state", file=out)
        for i in initial_state:
            print(i, file=out)
        print("end_state", file=out)

        print("begin_numeric_state", file=out)
        for i in initial_numeric_state:
            print(i, file=out)
        print("end_numeric_state", file=out)
        print("begin_goal", file=out)
        for g in goal:
            print(g, file=out)
        print("end_goal", file=out)

        print(len(operators), file=out)
        for op in operators:
            print("begin_operator", file=out)
            print(op['name'], file=out)
            print(op['num_preconditions'], file=out)
            for pr in op['preconditions']:
                print(pr, file=out)
            print(op['num_ass_effects'], file=out)
            for ef in op['ass_effects']:
                print(ef, file=out)
            print(op['num_effects'], file=out)
            for ef in op['effects']:
                print(ef, file=out)
            print(op['cost'], file=out)
            print("end_operator", file=out)

        print(len(rules), file=out)
        for rule in rules:
            print("begin_rule", file=out)
            print(int(rule[0]), file=out)
            for i in range(int(rule[0])):
                print(rule[1 + i], file=out)

            print(rule[-1], file=out)
            print("end_rule", file=out)

        print(len(axioms['comparison']), file=out)
        print("begin_comparison_axioms", file=out)
        for ax in axioms['comparison']:
            print(ax, file=out)
        print("end_comparison_axioms", file=out)
        print("""0
begin_numeric_axioms""", file=out)
        #for axiom in axioms['numeric']:
        #    print(axiom)
        print("""end_numeric_axioms""", file=out)
        #for key in formulas.keys():
        #    print(key, formulas[key])
        print("begin_global_constraint", file=out)
        for global_constraint in global_constraints:
            print(global_constraint, file=out)
        print("end_global_constraint", file=out)

print_result("output.sas")
print_result("output_lmaooo.sas")
