# Text editor

Open a text file from the Project files panel. The built-in editor provides line
numbers and syntax highlighting for C, C++, JSON, Lua, JavaScript (`.js`,
`.mjs`, `.cjs`), Ruby (`.rb`, `.rake`, `.gemspec`), PHP (`.php`, `.phtml`),
and assembly files (`.asm`, `.s`, and `.z80`). An enabled file-handler plugin takes precedence for file types
it supports.

Editor colors and text size are under **Settings → UI → Editor**. Text size ranges
from 8 to 48. Hold Ctrl and scroll over the editor to change it; SRZ80 saves the
new size after scrolling stops. This setting is independent of application scale
and tool-specific editor preferences.

The same settings group has options to show spaces and tabs, mark line endings,
and highlight the cursor's line. These affect the built-in editor display only.

## External changes

While the project session is idle, SRZ80 checks open source files about once per
second.

- A clean document reloads after another application saves or replaces its file.
  Reloading clears its undo/redo history.
- A document with local edits keeps its buffer and shows an external-change
  notice.
- Deleted, unreadable, binary, or oversized files never replace the open buffer.

Save checks the files again. If disk contents changed, the **File save conflict**
dialog lists the affected files:

- **Keep my edits and save** writes the editor versions. Deleted files are
  recreated when their parent directory still exists. A later disk change asks
  again.
- **Reload disk** discards local edits in readable files and cancels the save.
- **Cancel** keeps the editor buffers without saving.

Reload and Cancel also stop a New, Open, or Quit operation that was waiting for
the save. An unreadable disk version cannot be overwritten from this dialog;
repair the file or cancel while retaining the editor buffer.

Saves use a snapshot of the contents, so edits made while a save is running remain
unsaved. Checks occur before tool save callbacks and before each source replacement.
They cannot lock out another application or make native tool side effects part of
the same transaction.
