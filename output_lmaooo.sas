begin_version
4
end_version
begin_metric
< 10
end_metric
11
begin_variable
var0
11
2
Atom new-axiom@0()
NegatedAtom new-axiom@0()
end_variable
begin_variable
var1
-1
2
Atom saved(p0)
NegatedAtom saved(p0)
end_variable
begin_variable
var2
-1
2
Atom saved(p1)
NegatedAtom saved(p1)
end_variable
begin_variable
var3
10
3
>= 11 8
< 11 8
<none of those>
end_variable
begin_variable
var4
10
3
>= 9 8
< 9 8
<none of those>
end_variable
begin_variable
var5
10
3
<= 19 8
> 19 8
<none of those>
end_variable
begin_variable
var6
10
3
<= 17 8
> 17 8
<none of those>
end_variable
begin_variable
var7
10
3
>= 0 8
< 0 8
<none of those>
end_variable
begin_variable
var8
10
3
>= 14 8
< 14 8
<none of those>
end_variable
begin_variable
var9
10
3
<= 1 8
> 1 8
<none of those>
end_variable
begin_variable
var10
10
3
<= 6 8
> 6 8
<none of those>
end_variable
15
begin_numeric_variables
R 7 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE d(?t)(b0, b0, p1)
R 9 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE derived!sum_PNE derived!25.0()_PNE d(?t)(?t)(b0, b0, p1)
C -1 PNE derived!1.0()
R 5 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE derived!sum_PNE derived!25.0()_PNE d(?t)(?t)(b0, b0, p1)
C -1 PNE derived!0.0()
R 2 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE d(?t)(b0, b0, p0)
R 6 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE d(?t)(b0, b0, p0)
R 3 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE d(?t)(b0, b0, p1)
R 4 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE derived!sum_PNE derived!25.0()_PNE d(?t)(?t)(b0, b0, p0)
R 8 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE derived!sum_PNE derived!25.0()_PNE d(?t)(?t)(b0, b0, p0)
I -1 PNE total-cost()
C -1 !derived3.0from0 : [-110.0, 1, 1]
C -1 !derived-2.0from0 : [-110.0, 1, 1]
C -1 !derived-4.0from0 : [-110.0, 1, 1]
C -1 !derived-3.0from0 : [-110.0, 1, 1]
end_numeric_variables
0
begin_state
1
1
1
2
2
2
2
2
2
2
2
end_state
begin_numeric_state
-103.0
-128.0
1.0
-142.0
0.0
-39.0
-25.0
-117.0
-64.0
-50.0
0.0
3.0
-2.0
-4.0
-3.0
end_numeric_state
begin_goal
2
1 0
2 0
end_goal
9
begin_operator
go_est b0
0
0
9
0 10 + 2
0 0 + 11
0 1 + 11
0 3 + 14
0 5 + 14
0 6 + 11
0 7 + 14
0 8 + 14
0 9 + 11
1.0
end_operator
begin_operator
go_north_east b0
0
0
5
0 10 + 2
0 0 + 11
0 1 + 11
0 6 + 11
0 9 + 11
1.0
end_operator
begin_operator
go_north_west b0
0
0
5
0 10 + 2
0 3 + 11
0 5 + 11
0 7 + 11
0 8 + 11
1.0
end_operator
begin_operator
go_south b0
0
0
9
0 10 + 2
0 0 + 12
0 1 + 12
0 3 + 12
0 5 + 12
0 6 + 12
0 7 + 12
0 8 + 12
0 9 + 12
1.0
end_operator
begin_operator
go_south_east b0
0
0
5
0 10 + 2
0 0 + 13
0 1 + 13
0 6 + 13
0 9 + 13
1.0
end_operator
begin_operator
go_south_west b0
0
0
5
0 10 + 2
0 3 + 13
0 5 + 13
0 7 + 13
0 8 + 13
1.0
end_operator
begin_operator
go_west b0
0
0
9
0 10 + 2
0 0 + 14
0 1 + 14
0 3 + 11
0 5 + 11
0 6 + 14
0 7 + 11
0 8 + 11
0 9 + 14
1.0
end_operator
begin_operator
save_person b0 p0
4
3 0
4 0
5 0
6 0
1
0 1 -1 0
1
0 10 + 2
1.0
end_operator
begin_operator
save_person b0 p1
4
7 0
8 0
9 0
10 0
1
0 2 -1 0
1
0 10 + 2
1.0
end_operator
1
begin_rule
0
0 1 0
end_rule
8
begin_comparison_axioms
3 >= 6 4
4 >= 5 4
5 <= 9 4
6 <= 8 4
7 >= 0 4
8 >= 7 4
9 <= 1 4
10 <= 3 4
end_comparison_axioms
0
begin_numeric_axioms
end_numeric_axioms
begin_global_constraint
0 0
end_global_constraint
