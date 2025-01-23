import re
import sys
from logging import exception

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


def replace_var(var1, var2):
    for i in range(1, len(axioms['comparison'])):
        vals = axioms['comparison'][i].split()
        for val in range(len(vals)):
            if vals[val] == str(var1):
                vals[val] = var2
        axioms['comparison'][i].join(" ")

    for i in range(len(axioms['numeric'])):
        vals = axioms['numeric'][i].split()
        for val in range(len(vals)):
            if vals[val] == str(var1):
                vals[val] = var2
        axioms['numeric'][i].join(" ")


#We want to make a formula for each numeric var according to numeric axiom tree
#We store the formula in the following way:
#[(c0, const), (c1, rv1), (c2, rv2), ... (cn, real_variable_n)]
#for +/- we simply add coefficents for each variable respectively
#for * we search for non-zero coef near any variable and multiply this formula by coef of the other one
#there can be only 1 formula out of the 2 with non-zero coef near rv in it, since we oly work with linears here
#ROOM FOR IMPROVEMENT: we maybe can re-use formulas

formulas = {}


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
        try:
            i = real_numeric_variables.index(var)
            f[i + 1] = 1
        except ValueError:
            print("UEBISHE LESNOE")
            pass  # Handle the case where var is not found
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
from collections import defaultdict


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

    queue = [node for node in predecessors_list if in_degree[node] == 0]
    result = []

    #print(graph)
    #print(queue)
    while queue:
        node = queue.pop(0)
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


def get_axiom_by_var_final(var):
    for i in range(len(axioms['numeric'])):
        axiom = axioms['numeric'][i]
        var_final = int(axiom.split()[0])
        if (var_final == var):
            return i
    return -1


for i in range(len(sorted_nodes)):
    ax_num = get_axiom_by_var_final(sorted_nodes[i])
    if (ax_num == -1):
        continue
    gen_all_formulas_for_all_vars_met_in_axiom(axioms['numeric'][ax_num])

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

    initial_numeric_state[var] += total_initial_upd_value

    for operator in operators:
        total_effect_upd_value = 0
        for effect in operator['effects']:

            var1 = int(effect.split()[1])
            var2 = int(effect.split()[3])
            op = effect.split()[2]
            #if(var == 2):
            #    print(effect, var1, (var1 in real_numeric_variables), op)
            if var1 not in real_numeric_variables:
                continue
            if (op == "+"):
                total_effect_upd_value += formula[real_numeric_variables.index(var1) + 1] * get_var_value(var2)
            elif op == "-":
                total_effect_upd_value -= formula[real_numeric_variables.index(var1) + 1] * get_var_value(var2)
            else:
                raise Exception("Encountered assignment effect")
                #print("SUKA BLYAT UMNOZHENIE EBANOE")
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
    #update_var_with_formula(var1)
    #update_var_with_formula(var2)
    numeric_vars[var1] = "R" + numeric_vars[var1][1:]
    if var1 not in formulas.keys():
        formula = gen_initial_formula(var1)
        formulas[var1] = formula

    upd_val = get_var_value(var2) - formulas[var1][0]
    if upd_val in added_constants:
        axioms['comparison'][i] = " ".join(
            [str(axiom.split()[0]), str(axiom.split()[1]), str(var1), str(added_constants[upd_val])])
    else:
        added_constants[upd_val] = len(numeric_vars)
        numeric_vars.append("C -1 " + str(var2) + " " + str(upd_val))
        initial_numeric_state.append(get_var_value(var2) - formulas[var1][0])
        axioms['comparison'][i] = " ".join(
            [str(axiom.split()[0]), str(axiom.split()[1]), str(var1), str(len(numeric_vars) - 1)])
#print_all()

for i in range(len(numeric_vars)):
    if is_real_variable(i) and i not in real_numeric_variables:
        update_var_with_formula(i)


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

formula_to_var = {}
def duplicate_detect_formulas():
    flag_idx = []
    for var in range(len(numeric_vars)):
        if(is_real_variable(var)):
            continue
        if var in formulas.keys():
            formula = formulas[var]
        else:
            formula = gen_initial_formula(var)

        formula = " ".join(map(str, formula))
        if formula not in formula_to_var.keys():
            formula_to_var[formula] = var
            continue
        replace_var(var, formula_to_var[formula])
        #initial_numeric_state = initial_state[:var] + initial_numeric_state[var + 1:]
        flag_idx.append(numeric_vars[var])

    #print("Flag: ", flag_idx, file = out)

    new_numeric_vars = [var for var in numeric_vars if var not in flag_idx]
    new_initial_numeric_state = [val for i, val in enumerate(initial_numeric_state) if numeric_vars[i] not in flag_idx]
    return  new_numeric_vars, new_initial_numeric_state

#numeric_vars, initial_numeric_state = duplicate_detect_formulas()


def print_result(output_file):
    with open(output_file, "w") as out:
        print("begin_version", file=out)
        print(version, file=out)
        print("end_version", file=out)
        print("begin_metric", file=out)
        print(metric, file=out)
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