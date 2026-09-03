# Parameters and SCPT Expressions

Select an instruction to see its parameters. The parameter table has three
columns:

- **Parameter** identifies what the value does according to the SCT opcode
  schema.
- **Value** shows the editable semantic value.
- **Notes** preserves exact encoding details and may show advisory runtime
  research.

Runtime notes are research aids. They may be incomplete or incorrect and do
not impose validation, platform, or export rules.

## Editing parameter values

Double-click an editable Value cell. Press Enter to validate and commit the
change. Press Escape or move focus away to cancel it. Invalid input leaves the
editor open, explains the problem, and does not create an Undo step.

Each accepted parameter edit is one Undo/Redo step. Fixed parameters appear as
ordinary rows. Repeated groups appear as collapsible rows and provide commands
to add, remove, and reorder complete groups. A group count is derived from its
groups and is not edited directly.

Scalar fields use the signed, unsigned, or hexadecimal form established by
their schema. Confirmed bit constraints are enforced. Provisional constraints
produce warnings rather than silently changing the value.

Typed references select compatible instructions or strings; raw byte offsets
are never edited directly. Activating an indexed-string reference opens its
actual string in the SCT Text Editor. A one-line footer plain string is edited
as part of the instruction parameter. If imported instructions share that
string, editing creates a private copy for the active parameter so the other
instructions are unchanged.

Opaque parameter words remain visible but read-only. Use **Replace With Typed
Value** only when you intend to replace the complete opaque value with a known
semantic value.

## Conventional SCPT expressions

A conventional SCPT program appears as expression text directly in the Value
column. SALSA parses that text and creates the corresponding ordered SCPT
program when you press Enter.

Fixed decimals do not have a suffix:

- `3`
- `3.5`
- `-2.25`

They must be exactly representable in the signed 24-bit fixed-point domain at
1/256 precision. SALSA never guesses that an unsuffixed value is a float.

Finite IEEE-754 floats require an `f` suffix:

- `3f`
- `3.5f`
- `1e-3f`

Variable forms identify both the variable kind and index:

- `ByteVar[87]`
- `BitVar[4]`
- `FloatVar[12]`
- `IntVar[87]`
- `FloatBackedIntVar[24]`
- `Low16IntVar[15]`
- `IntInput[87]`

`IntInput` lets the SpiceSCT factory select the encoded integer-input form used
by the game for that index. `IntVar` converts a signed integer slot to a float,
while `FloatBackedIntVar` reads the same storage as float bits. The low-16 form
also selects the interpreter's low-16 comparison mode. The confirmed encoding
uses `FloatBackedIntVar` for indexes 24 through 32 and `Low16IntVar` for index
15; other integer-variable indexes use `IntVar` except indexes reserved for
named secondary values. Invalid form/index combinations are rejected.

Known runtime values use names such as:

- `Gold`
- `VyseCurrentHP`

The four confirmed inline constants use an explicit encoded form, for example
`InlineValue[0x7F7FFFFF]`.

Expressions can combine values with parentheses and these binary operators:

| Precedence | Operators | Meaning |
| ---: | --- | --- |
| 1 | `*` `/` `%` | multiply, divide, remainder |
| 2 | `+` `-` | add, subtract |
| 3 | `<` `<=` `>` `>=` | ordered comparison |
| 4 | `==` `!=` | equality comparison |
| 5 | `&` | bitwise AND |
| 6 | `\|` | bitwise OR |
| 7 | `&&` | logical AND |
| 8 | `\|\|` | logical OR |

Rows nearer the top bind more tightly. Operators with the same precedence are
left-associative. A leading sign is supported only on a numeric literal; there
is no general unary-expression syntax.

Examples:

- `ByteVar[87] == 3`
- `(FloatBackedIntVar[24] + 2) * 4`
- `IntVar[87] == 1 && ByteVar[87] != 0`

Long expressions scroll horizontally while editing. Hover the cell to see the
complete expression and its exact encoded words.

## When to use the Advanced SCPT Editor

Inline text is available only when every ordered operation contributes exactly
once to one conventional result. Open the Advanced SCPT Editor for stack
overwrite operations, inert words, residual stack values, unusual postfix
programs, or any other nonconventional program.

The advanced editor shows:

- a semantic-result preview when one can be derived;
- an ordered table of semantic operations, operands, exact words, and stack
  effects;
- grouped commands for values, comparisons, arithmetic, bitwise, logical, and
  stack operations.

Existing inert operations remain byte-exact and can be moved or removed, but
there is no normal command to add new inert data. Alternate known operator
encodings remain visible in the operation details.

For nonconventional programs, the ordered operation list is authoritative. The
semantic result is marked as a preview and is not an exact editable
representation. **Convert to Conventional Expression** explicitly replaces
the complete ordered draft with the canonical program for the entered text.
Applying the draft creates one Undo/Redo step.

Scheduled expressions are not editable through this workflow yet.
