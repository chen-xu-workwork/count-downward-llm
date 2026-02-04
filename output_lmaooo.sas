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
>= 14 7
< 14 7
<none of those>
end_variable
begin_variable
var4
10
3
>= 5 7
< 5 7
<none of those>
end_variable
begin_variable
var5
10
3
<= 19 7
> 19 7
<none of those>
end_variable
begin_variable
var6
10
3
<= 15 7
> 15 7
<none of those>
end_variable
begin_variable
var7
10
3
>= 3 7
< 3 7
<none of those>
end_variable
begin_variable
var8
10
3
>= 18 7
< 18 7
<none of those>
end_variable
begin_variable
var9
10
3
<= 17 7
> 17 7
<none of those>
end_variable
begin_variable
var10
10
3
<= 1 7
> 1 7
<none of those>
end_variable
15
begin_numeric_variables
R 5 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE derived!sum_PNE d(?t)_PNE derived!25.0()(?t)(b0, b0, p1)
R 7 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE d(?t)(b0, b0, p1)
R 2 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE d(?t)(b0, b0, p0)
C -1 PNE derived!0.0()
R 6 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE d(?t)(b0, b0, p0)
R 4 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE derived!sum_PNE d(?t)_PNE derived!25.0()(?t)(b0, b0, p0)
C -1 PNE derived!1.0()
R 9 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE derived!sum_PNE d(?t)_PNE derived!25.0()(?t)(b0, b0, p1)
R 3 PNE derived!difference_PNE derived!difference_PNE y(?b)_PNE x(?b)(?b, ?b)_PNE d(?t)(b0, b0, p1)
R 8 PNE derived!difference_PNE derived!sum_PNE x(?b)_PNE y(?b)(?b, ?b)_PNE derived!sum_PNE d(?t)_PNE derived!25.0()(?t)(b0, b0, p0)
I -1 PNE total-cost()
C -1 !derived-3.0from1 : [-135.0, -1, 1]
C -1 !derived3.0from1 : [-135.0, -1, 1]
C -1 !derived-2.0from1 : [-135.0, -1, 1]
C -1 !derived-4.0from1 : [-135.0, -1, 1]
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
-142.0
-103.0
-39.0
0.0
-25.0
-64.0
1.0
-128.0
-117.0
-50.0
0.0
-3.0
3.0
-2.0
-4.0
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
0 10 + 6
0 0 + 11
0 1 + 12
0 2 + 11
0 4 + 12
0 5 + 11
0 7 + 12
0 8 + 11
0 9 + 12
1.0
end_operator
begin_operator
go_north_east b0
0
0
5
0 10 + 6
0 1 + 12
0 4 + 12
0 7 + 12
0 9 + 12
1.0
end_operator
begin_operator
go_north_west b0
0
0
5
0 10 + 6
0 0 + 12
0 2 + 12
0 5 + 12
0 8 + 12
1.0
end_operator
begin_operator
go_south b0
0
0
9
0 10 + 6
0 0 + 13
0 1 + 13
0 2 + 13
0 4 + 13
0 5 + 13
0 7 + 13
0 8 + 13
0 9 + 13
1.0
end_operator
begin_operator
go_south_east b0
0
0
5
0 10 + 6
0 1 + 14
0 4 + 14
0 7 + 14
0 9 + 14
1.0
end_operator
begin_operator
go_south_west b0
0
0
5
0 10 + 6
0 0 + 14
0 2 + 14
0 5 + 14
0 8 + 14
1.0
end_operator
begin_operator
go_west b0
0
0
9
0 10 + 6
0 0 + 12
0 1 + 11
0 2 + 12
0 4 + 11
0 5 + 12
0 7 + 11
0 8 + 12
0 9 + 11
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
0 10 + 6
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
0 10 + 6
1.0
end_operator
1
begin_rule
0
0 1 0
end_rule
8
begin_comparison_axioms
3 >= 4 3
4 >= 2 3
5 <= 9 3
6 <= 5 3
7 >= 1 3
8 >= 8 3
9 <= 7 3
10 <= 0 3
end_comparison_axioms
0
begin_numeric_axioms
end_numeric_axioms
begin_global_constraint
0 0
end_global_constraint
