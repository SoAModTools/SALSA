SALSA - Skies of Arcadia Legends Script Assistant
===================================================
SALSA is a GUI-based tool to assist in editing sct files in Skies of Arcadia Legends. Currently, SALSA allows for .sct file import, export, and testing in Dolphin

Requirements
------------
* [Python](https://www.python.org/) 3.10+
* Skies of Arcadia Legends script files (.sct).
  * Supports aklz compressed files

Python Dependencies
-------------------
* [psutil](https://github.com/giampaolo/psutil)
  * For testing scripts by sending the current one to Dolphin as a live update

Getting Started
---------------
With Python 3.10+ installed, running SALSA.py should run the program. In the event that this does not start the program, make sure it is running with Python 3.10+.

Upon startup, a project should first be created or loaded. A project can contain an arbitrary number of scripts. Scripts can be imported using Project->Import Script, and exported using Project-> Export Script. If a script is imported, but a script with the same name is already present, the imported script will overwrite the current one. A project can also be saved for later use.

Features (Completed)
--------------------
* Instructions with editable parameter names and default values
* Parameter values can be displayed within instruction descriptions
* Script file import and export
* Script file addressing by setting starting memory address for script file for use with Dolphin Emulator. (right-click headers and include offset to view memory address)
* Dialog string editing (Project->String Editor)
* Moving instructions around within a section
* Grouping and moving sections around within a script
* Ability to navigate using instruction detail links
* Adding, removing, and changing instructions
* Editing parameter values
* Ability to name script variables (Variable alias editor)

* NEW: Ability to send scripts to Dolphin when running Skies of Arcadia Legends
  * Only works on Windows at the moment and requires psutil

* NEW: Instructions can be disabled from encoding
  * This effectively causes the encoder to "skip" these instructions when encoding
  * Instructions can be disabled via the right click menu and will have a "//" prefix when disabled

Features (In Progress)
----------------------
* Program help (Opens into a webpage. Only skeleton is implemented)

Features (Planned)
------------------
* Saving groups of sections separately
* Saving groups of instructions separately

Using Dolphin Link
------------------
Dolphin Link allows SALSA to send scripts to a running instance of Dolphin. This is best used when Dolphin is run with the debugging UI.

To use prepare for sending a new version of the script, first run Skies of Arcadia Legends in Dolphin, then press the 'Connect to Dolphin' button. While Skies of Arcadia remains running, the current script and ptr to the current instruction will be displayed as well. To send your version of the current script in Dolphin, the game should be paused (No guarantees if the game is not paused). It is best if it is paused in the Main function. To do this, on the code tab of Dolphin, first click on the second to last entry in the call stack box. Then set a breakpoint at the instruction immediately following the current one by clicking in the column to the left of the address column. Then press play on Dolphin again. It should stop when that breakpoint is hit and the breakpoint can be removed.

To update the current SCT in Dolphin, press the 'Update Current SCT' button. If you have a script with the same name as the current script in Dolphin, SALSA will encode your script into the sct format. Since the size of the updated script will almost always be different, just sending the updated script would cause a desync in the script interpreter, so SALSA needs to set the location of the next scheduled inst as well. To do this, SALSA will try to find a similar instruction to the one Dolphin has scheduled. This will only work if: 1) there is a section with the same name as the currently running section, and 2) there is an instruction with the same instruction ID and parameter values in that section. If SALSA is unable to find an appropriate instruction to set, the currently selected instruction in SALSA can be sent to Dolphin instead by using the checkbox under the update button.

In addition to the scheduled instruction, SALSA also needs to update the section callstack. Every time instruction 11 (go to subscript), the next position is put on the section callstack. Therefore, for an update to be successful, there needs to be a chain of go to subscripts similar to in game. Since SALSA detects these by section name, the names would need to be identical to the current script in the game. If SALSA detects that one is missing, an error will be produced. Notable exceptions are when in the init or loop (when walking or flying around) sections, there are no calls in the subscript callstack, so this does not apply and changes to section names can safely be made.

Credits
-------
See [CREDITS.md](/CREDITS.md) for full credits

License
-------
Copyright(C) 2023. SALSA is licensed under the GNU General Public License, Version 3.0. See [LICENSE.md](/LICENSE.md) for full license text
