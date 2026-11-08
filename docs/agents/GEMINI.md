# Repository Rules: Open-K2 Firmware

## Git Commit Standards

All commits in this repository must follow strict technical documentation standards:

1. **Header Format**: `<type>(<scope>): <summary>` (max 72 chars, imperative mood).
2. **Body Structure**:
   * **Summary**: Clear summary of what changed.
   * **Rationale**: Problem statement, reverse engineering findings, and justification for the change.
   * **Details**: Explicit register names, addresses, bits, and formulas.
3. **Co-Authorship Attribution**:
   * Append standard Git co-authorship trailer attributing the model:
     ```git
     Co-authored-by: Gemini 3.8 Flash <antigravity@google.com>
     ```
