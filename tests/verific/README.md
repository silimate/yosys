# Verific Test Cases

## Disabled

- `bounds`: checks top and bottom bound attributes, which are removed to avoid OpenSTA issues
- `memory_semantics`: relies on initial values being retained, which we do not want
- `rom_case`: relies on using Verific's VHDL frontend rather than GHDL
