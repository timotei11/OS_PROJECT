# AI_usage-All_Phases.md

## Tool Used
Claude (claude.ai)

---

## Phase 1

**parse_condition**

Prompt: asked Claude to write a function that splits a string like
`severity:>=:2` into field, operator and value.

What was generated: used strchr() to find the colons and pointer
arithmetic to get the lengths. It worked fine.

What I changed: nothing, just added some comments.

What I learned: you can subtract two pointers to get a string length,
didn't know that before.

---

**match_condition**

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

## Phase 2

**monitor_reports.c**

Prompt: asked Claude for a working example of sigaction() usage and
the pause() loop pattern, since I hadn't used either before.

What I changed: integrated the pattern into my own program structure,
added fflush(stdout) after printf calls so output appears immediately.

What I learned: pause() suspends the process until a signal arrives
without busy-waiting. volatile sig_atomic_t is needed for flags shared
between the main loop and signal handlers.

---

**remove_district**

Prompt: asked Claude how to use fork() and execvp() to run an
external command and wait for it to finish.

What I changed: added the manager-only role check and the symlink
cleanup after the directory was removed.

---

## Phase 3

**monitor_reports.c**

Prompt: asked Claude to help add duplicate monitor detection and a
structured message format so hub_mon can parse monitor output over a pipe.

What I learned: kill(pid, 0) doesn't send a real signal — it only
checks if the process exists.

---

**scorer.c**

Prompt: asked Claude to write the scorer program that reads
reports.dat, sums severities per inspector, and prints results to stdout.



**city_hub.c**

Prompt: asked Claude to help implement the hub with start_monitor
and calculate_scores using fork(), pipe(), and dup2().

**What I changed:** added detection of both QUIT and ERROR as shutdown
signals in hub_mon, and the check for an already-running hub_mon before
starting a new one.