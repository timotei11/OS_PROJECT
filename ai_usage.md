# ai_usage.md — AI-Assisted Filter Functions

## Context

This file documents the use of AI assistance for the two helper functions
in the `filter` command, as explicitly permitted by the project specification.

---

## Tool Used

**Claude (claude-sonnet-4-6)** via the claude.ai web interface.

---

## Prompts Given and What Was Generated

### Function 1: `parse_condition`

**Prompt:**
> "I have a C struct called `Report` and a filter command that receives
> conditions as strings in the format `field:operator:value`, for example
> `severity:>=:2` or `category:==:road`. Write a function:
> `int parse_condition(const char *input, char *field, char *op, char *value);`
> that splits the string into the three parts. Field, op, and value are
> output parameters. Return 1 on success, 0 if the string is malformed."

**What was generated:**
The AI generated a function using `strchr()` to find the two `:` separators,
computing the lengths of each segment, using `strncpy` for the first two parts
and `strcpy` for the remainder (value). The logic was clean and correct.

**What I changed:**
Nothing structural — the logic was correct. I added a comment explaining each
step so I can describe it at the presentation.

**What I learned:**
`strchr()` returns a pointer to the first occurrence of the character, which
makes pointer arithmetic (`p2 - (p1 + 1)`) a clean way to measure the
operator's length without any loop.

---

### Function 2: `match_condition`

**Prompt:**
> "Using the same Report struct (fields: int id, char inspector[64],
> double lat, double lon, char category[32], int severity, time_t timestamp,
> char description[80]), write a function:
> `int match_condition(Report *r, const char *field, const char *op, const char *value);`
> that returns 1 if the record satisfies the condition, 0 otherwise.
> Supported fields: severity, category, inspector, timestamp.
> Supported operators: ==, !=, <, <=, >, >=."

**What was generated:**
A chain of `strcmp(field, ...)` checks, with nested `strcmp(op, ...)` for each
operator. For integer fields (severity) it used `atoi()`. For string fields it
used `strcmp()`.

**What I changed / what I had to fix:**
1. The AI initially **omitted the `timestamp` branch** entirely. I added it
   myself, using `atol()` (not `atoi()`) because `time_t` is a `long` on
   64-bit Linux and epoch values do not fit in an `int`.
2. The AI applied `<`, `<=`, `>`, `>=` to string fields, which is technically
   valid C but meaningless for category/inspector names. I removed those
   operators from string fields and added a comment explaining why.
3. The AI did not include a fallback `else` for unknown field names. I added
   a `fprintf(stderr, "Warning: unknown filter field...")` branch.

**What I learned:**
- `atoi()` silently truncates large Unix timestamps — always use `atol()` for
  `time_t`.
- AI-generated code often handles the "happy path" well but skips edge cases
  like unknown operators, unknown fields, or type overflow. These must always
  be reviewed manually.
- The spec says "review both functions line by line" — this exercise shows why:
  the timestamp omission would have caused a silent compile error or crash at
  runtime.

---

## Critical Evaluation

| Aspect                         | Assessment |

| Correctness                    | Mostly correct; timestamp branch missing |
| Completeness                   | Good for the happy path; missing edge cases |
| Safety                         | No buffer overflows; uses strncpy appropriately |
| Clarity                        | Clean, readable code |
| What required my own judgement |Type selection(atol vs atoi), removing nonsensical
                                  operators from string fields, adding error fallback |

The functions were a useful starting point, but required non-trivial review
and modification before they were production-ready. This matches the spec's
intent: AI as a tool for accelerating boilerplate, not a replacement for
understanding.