# wio-graphical-data-logger
Arduino data logger with line chart diagram and file handling in a graphical user interface.

It runs on a Wio Terminal with an analogue Seed Studio Grove sensor connected in the right socket and optionally a speaker in the left socket. (Adjusting pin numbers might perhaps make it run on other hardware with a display.)

## Overview of the GUI
On the live screen, the up & down arrows and middle button control a threshold value. The ratio between the measured value and the threshold is shown by a vertical bar on
at the right edge of the screen, and also affects the background colour of the entire screen.
In case sound is enabled, the speaker starts beeping at a rate relating to how
much the threshold is exceeded.

On the live screen, the left & right arrows control the sampling rate, i.e. the number
of samples that are averaged to produce one data point. Actually, a fixed pattern of
several raw analogue readouts are made even per sample, and median-values are used in
order to suppress some of the high-frequency noise that cheap switching power supplies create.

On the Menu screen, the up & down arrows allow selecting a measurement scale,
i.e. how the raw voltage measurements should be transformed for display (and logging).
See the valueFromRaw() function and the variable named sample_transform.
At start-up, the default sample_transform converts readings from the 
Grove GSR (galvanic skin response) sensor 101020052 to conductance (inverse of resistance),
and the Grove moisture sensor 101020008 behaves similarly. For general use, there is
a sample_transform option which simply expresses the analogue input as a voltage.

## File management
If an SD-card is available, data can be logged to a text file with a tab-separated values.
Each row contains three columns: a timestamp in seconds, the measured value, and the threshold setting.
The program contains a file browser for navigating and visualizing a logged file,
including the ability to zoom and scroll within the file's diagram. A file can
also be exported to the virtual serial port (a computer connected via USB),
as alternative to moving the SD-card to a computer with card reader.
At least some 32 GB cards work although some (old) documentation says max 16 GB.

The files named with automatically incrementing five-digit filenames, organized
in four-letter directories. Rebooting the Wio Terminal means that the next file
will get a new directory, and that the timesetamp clock is restarts at 0.

## Installation notes
For Arduino IDE to open the program, the repository folder needs to be renamed to "wio_graphical_data_logger".

This project depends on the following non-standard-Arduino libraries:
"Seeed Arduino FS" https://wiki.seeedstudio.com/Wio-Terminal-FS-Overview/
"Seeed_Arduino_Linechart" https://wiki.seeedstudio.com/Wio-Terminal-LCD-Linecharts/
The version of the TFT_eSPI library provided out-of-the-box by "Seeduino Wio Terminal"
board in Arduino IDE! Thus you should NOT use the library manager to install TFT_eSPI
(https://wiki.seeedstudio.com/Wio-Terminal-LCD-Graphics/). Otherwise nothing but the screen backlight works!
