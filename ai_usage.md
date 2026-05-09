AI_usage-phases_1_and_2.md

Tool Used
Claude (claude.ai)

---

Phase 1

I used Claude for the two filter functions as allowed by the spec.

parse_condition

Prompt: asked Claude to write a function that splits a string like
`severity:>=:2` into field, operator and value.

What was generated: used strchr() to find the colons and pointer
arithmetic to get the lengths. It worked fine.

What I changed: nothing, just added some comments.

What I learned: you can subtract two pointers to get a string length,
didn't know that before.

---

match_condition

Prompt: asked Claude to write a function that checks if a Report record
matches a condition (field, operator, value).

What was generated: a chain of strcmp() checks for each field. Used
atoi() for severity, strcmp() for strings.

What I had to fix:
1. timestamp branch was missing — added it myself, also changed atoi() to
   atol() because time_t is a long and timestamps overflow an int
2. had <, > operators on string fields which makes no sense — removed them
3. no warning for unknown fields — added one myself

What I learned: atoi() silently overflows on timestamps. AI handles
the easy cases but misses type-related edge cases.

---

Phase 2

Wrote most of the Phase 2 code myself. I asked Claude to help me with
the signal handler setup in monitor_reports.c — specifically how to use
sigaction() correctly, since I hadn't used it before. 